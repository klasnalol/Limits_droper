#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QToolButton>
#include <QSettings>
#include <QSet>
#include <QStyle>
#include <QSize>
#include <QSpinBox>
#include <QStandardPaths>
#include <QString>
#include <QVBoxLayout>
#include <QtGlobal>
#include <vector>

#include <cinttypes>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uint32_t kMsrPkgPowerLimit = 0x610;
constexpr std::uint32_t kMchbarPlOffset = 0x59A0;
constexpr double kUvMvScale = 1.024;
constexpr double kMinFontScale = 0.8;

double quantize_uv_mv(double mv) {
    return std::llround(mv * kUvMvScale) / kUvMvScale;
}

std::uint64_t apply_pl_units(std::uint64_t cur, std::uint16_t pl1_units, std::uint16_t pl2_units) {
    std::uint32_t lo = static_cast<std::uint32_t>(cur & 0xffffffffu);
    std::uint32_t hi = static_cast<std::uint32_t>(cur >> 32);

    lo = (lo & ~0x7FFFu) | (static_cast<std::uint32_t>(pl1_units) & 0x7FFFu);
    hi = (hi & ~0x7FFFu) | (static_cast<std::uint32_t>(pl2_units) & 0x7FFFu);

    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

QString hex64(std::uint64_t v) {
    return QString("0x%1").arg(v, 16, 16, QLatin1Char('0'));
}

QString units_to_text(std::uint16_t units, double unit_watts) {
    double watts = static_cast<double>(units) * unit_watts;
    return QString("units %1 (%2 W)").arg(units).arg(watts, 0, 'f', 2);
}

struct CpuInfo {
    QString vendor;
    QString model_name;
    QString family;
    QString model;
    QString stepping;
    QString microcode;
    QString cache_size;
    int logical_cpus = 0;
    int packages = 0;
    int physical_cores = 0;
    double min_mhz = 0.0;
    double max_mhz = 0.0;
};

QString read_text_file(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

double read_khz_to_mhz(const QString &path) {
    QString text = read_text_file(path);
    bool ok = false;
    qlonglong khz = text.toLongLong(&ok);
    if (!ok || khz <= 0) {
        return 0.0;
    }
    return static_cast<double>(khz) / 1000.0;
}

CpuInfo read_cpu_info() {
    CpuInfo info;
    QString data = read_text_file("/proc/cpuinfo");
    if (data.isEmpty()) {
        return info;
    }

    QStringList lines = data.split('\n');
    bool first = true;
    int pkg_id = -1;
    int core_id = -1;
    QSet<int> packages;
    QSet<QString> cores;

    auto flush_core = [&]() {
        if (pkg_id >= 0 && core_id >= 0) {
            packages.insert(pkg_id);
            cores.insert(QString("%1:%2").arg(pkg_id).arg(core_id));
        }
        pkg_id = -1;
        core_id = -1;
    };

    for (const QString &line : lines) {
        if (line.isEmpty()) {
            continue;
        }
        int idx = line.indexOf(':');
        if (idx <= 0) {
            continue;
        }
        QString key = line.left(idx).trimmed();
        QString value = line.mid(idx + 1).trimmed();

        if (key == "processor") {
            info.logical_cpus += 1;
            flush_core();
            continue;
        }
        if (key == "physical id") {
            bool ok = false;
            int v = value.toInt(&ok);
            if (ok) {
                pkg_id = v;
            }
            continue;
        }
        if (key == "core id") {
            bool ok = false;
            int v = value.toInt(&ok);
            if (ok) {
                core_id = v;
            }
            continue;
        }

        if (first) {
            if (key == "vendor_id") {
                info.vendor = value;
            } else if (key == "model name") {
                info.model_name = value;
            } else if (key == "cpu family") {
                info.family = value;
            } else if (key == "model") {
                info.model = value;
            } else if (key == "stepping") {
                info.stepping = value;
            } else if (key == "microcode") {
                info.microcode = value;
            } else if (key == "cache size") {
                info.cache_size = value;
            }
        }
    }
    flush_core();

    if (!packages.isEmpty()) {
        info.packages = packages.size();
    }
    if (!cores.isEmpty()) {
        info.physical_cores = cores.size();
    }

    info.min_mhz = read_khz_to_mhz("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
    info.max_mhz = read_khz_to_mhz("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");

    if (info.packages == 0 && info.logical_cpus > 0) {
        info.packages = 1;
    }
    if (info.physical_cores == 0 && info.logical_cpus > 0) {
        info.physical_cores = 0;
    }

    return info;
}

double read_current_mhz_for_cpu(int cpu) {
    QString base = QString("/sys/devices/system/cpu/cpu%1/cpufreq/").arg(cpu);
    double mhz = read_khz_to_mhz(base + "scaling_cur_freq");
    if (mhz <= 0.0) {
        mhz = read_khz_to_mhz(base + "cpuinfo_cur_freq");
    }
    return mhz;
}

QList<int> parse_cpu_list(const QString &list) {
    QList<int> out;
    if (list.isEmpty()) {
        return out;
    }
    const QStringList parts = list.split(',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        int cpu = part.trimmed().toInt(&ok);
        if (ok) {
            out.append(cpu);
        }
    }
    return out;
}

QString format_mhz_stats(const QList<int> &cpus) {
    if (cpus.isEmpty()) {
        return "-";
    }
    double sum = 0.0;
    double minv = 0.0;
    double maxv = 0.0;
    int count = 0;
    for (int cpu : cpus) {
        double mhz = read_current_mhz_for_cpu(cpu);
        if (mhz <= 0.0) {
            continue;
        }
        if (count == 0) {
            minv = mhz;
            maxv = mhz;
        } else {
            if (mhz < minv) minv = mhz;
            if (mhz > maxv) maxv = mhz;
        }
        sum += mhz;
        count++;
    }
    if (count == 0) {
        return "-";
    }
    double avg = sum / count;
    return QString("avg %1 (min %2 / max %3)")
        .arg(avg, 0, 'f', 0)
        .arg(minv, 0, 'f', 0)
        .arg(maxv, 0, 'f', 0);
}

struct ReadState {
    int power_unit = 0;
    double unit_watts = 0.0;
    std::uint64_t msr = 0;
    std::uint64_t mmio = 0;
    bool core_type_supported = false;
    QString p_cpus;
    QString e_cpus;
    QString u_cpus;
    bool p_ratio_valid = false;
    bool e_ratio_valid = false;
    int p_ratio = 0;
    int e_ratio = 0;
    bool p_ratio_cur_valid = false;
    bool e_ratio_cur_valid = false;
    int p_ratio_cur = 0;
    int e_ratio_cur = 0;
    bool core_uv_valid = false;
    double core_uv_mv = 0.0;
    QString core_uv_raw;
};

} // namespace

class CollapsibleSection : public QFrame {
public:
    explicit CollapsibleSection(const QString &title, QWidget *content, int spacing, QWidget *parent = nullptr)
        : QFrame(parent), content_(content) {
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Plain);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(spacing, spacing / 2, spacing, spacing);
        layout->setSpacing(spacing / 2);

        toggle_ = new QToolButton();
        toggle_->setCheckable(true);
        toggle_->setChecked(true);
        toggle_->setArrowType(Qt::DownArrow);
        toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle_->setText(title);
        toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto *header = new QHBoxLayout();
        header->setSpacing(spacing / 2);
        header->addWidget(toggle_);
        header->addStretch();

        layout->addLayout(header);
        if (content_) {
            layout->addWidget(content_);
            content_->setVisible(true);
        }

        connect(toggle_, &QToolButton::toggled, this, [this](bool on) {
            if (content_) {
                content_->setVisible(on);
            }
            toggle_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        });
    }

    void setExpanded(bool on) {
        toggle_->setChecked(on);
    }

    bool isExpanded() const {
        return toggle_->isChecked();
    }

    QToolButton *toggleButton() const {
        return toggle_;
    }

private:
    QToolButton *toggle_ = nullptr;
    QWidget *content_ = nullptr;
};

class HelperBackend {
public:
    HelperBackend() : helper_path_(resolve_helper_path()) {}

    bool helper_available(QString *err) const {
        QFileInfo info(helper_path_);
        if (!info.exists()) {
            if (err) {
                *err = QString("Helper not found at %1. Install to /usr/local/bin, or set LIMITS_HELPER_PATH and update the polkit policy path.")
                           .arg(helper_path_);
            }
            return false;
        }
        if (!info.isExecutable()) {
            if (err) {
                *err = QString("Helper is not executable: %1").arg(helper_path_);
            }
            return false;
        }
        return true;
    }

    bool read_state(ReadState &state, QString *err) const {
        QString out;
        QString err_out;
        if (!run_pkexec({"--read"}, &out, &err_out)) {
            if (err) {
                *err = err_out.isEmpty() ? "Failed to run helper" : err_out;
            }
            return false;
        }
        return parse_state(out, state, err);
    }

    bool write_msr(std::uint64_t val, QString *err) const {
        return run_simple({"--write-msr", hex64(val)}, err);
    }

    bool write_mmio(std::uint64_t val, QString *err) const {
        return run_simple({"--write-mmio", hex64(val)}, err);
    }

    bool write_powercap(std::uint64_t pl1_uw, std::uint64_t pl2_uw, QString *err) const {
        return run_simple({"--write-powercap", QString::number(pl1_uw), QString::number(pl2_uw)}, err);
    }

    bool start_thermald(QString *err) const {
        return run_simple({"--start-thermald"}, err);
    }

    bool stop_thermald(QString *err) const {
        return run_simple({"--stop-thermald"}, err);
    }

    bool disable_thermald(QString *err) const {
        return run_simple({"--disable-thermald"}, err);
    }

    bool enable_thermald(QString *err) const {
        return run_simple({"--enable-thermald"}, err);
    }

    bool start_tuned(QString *err) const {
        return run_simple({"--start-tuned"}, err);
    }

    bool stop_tuned(QString *err) const {
        return run_simple({"--stop-tuned"}, err);
    }

    bool disable_tuned(QString *err) const {
        return run_simple({"--disable-tuned"}, err);
    }

    bool enable_tuned(QString *err) const {
        return run_simple({"--enable-tuned"}, err);
    }

    bool start_tuned_ppd(QString *err) const {
        return run_simple({"--start-tuned-ppd"}, err);
    }

    bool stop_tuned_ppd(QString *err) const {
        return run_simple({"--stop-tuned-ppd"}, err);
    }

    bool disable_tuned_ppd(QString *err) const {
        return run_simple({"--disable-tuned-ppd"}, err);
    }

    bool enable_tuned_ppd(QString *err) const {
        return run_simple({"--enable-tuned-ppd"}, err);
    }

    bool set_p_ratio(int ratio, QString *err) const {
        return run_simple({"--set-p-ratio", QString::number(ratio)}, err);
    }

    bool set_e_ratio(int ratio, QString *err) const {
        return run_simple({"--set-e-ratio", QString::number(ratio)}, err);
    }

    bool set_pe_ratio(int ratio_p, int ratio_e, QString *err) const {
        return run_simple({"--set-pe-ratio", QString::number(ratio_p), QString::number(ratio_e)}, err);
    }

    bool set_all_ratio(int ratio, QString *err) const {
        return run_simple({"--set-all-ratio", QString::number(ratio)}, err);
    }

    bool set_core_uv(double mv, QString *err) const {
        return run_simple({"--set-core-uv", QString::number(mv, 'f', 3)}, err);
    }

private:
    QString resolve_helper_path() const {
        QString env = qEnvironmentVariable("LIMITS_HELPER_PATH");
        if (!env.isEmpty()) {
            return env;
        }
        QString local = QCoreApplication::applicationDirPath() + "/limits_helper";
        if (QFileInfo::exists(local)) {
            return local;
        }
        return QStringLiteral("/usr/local/bin/limits_helper");
    }

    bool run_simple(const QStringList &args, QString *err) const {
        QString out;
        QString err_out;
        if (!run_pkexec(args, &out, &err_out)) {
            if (err) {
                *err = err_out.isEmpty() ? "Failed to run helper" : err_out;
            }
            return false;
        }
        return true;
    }

    bool run_pkexec(const QStringList &args, QString *out, QString *err) const {
        QProcess proc;
        proc.setProgram("pkexec");
        QStringList full_args;
        full_args << helper_path_;
        full_args << args;
        proc.setArguments(full_args);
        proc.start();

        if (!proc.waitForFinished(-1)) {
            if (err) {
                *err = "Helper timed out.";
            }
            return false;
        }

        if (out) {
            *out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        }
        QString err_text = QString::fromLocal8Bit(proc.readAllStandardError());

        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            if (err) {
                if (!err_text.isEmpty()) {
                    *err = err_text.trimmed();
                } else {
                    *err = QString("Helper failed (exit %1)").arg(proc.exitCode());
                }
            }
            return false;
        }

        if (err) {
            *err = err_text.trimmed();
        }
        return true;
    }

    bool parse_state(const QString &out, ReadState &state, QString *err) const {
        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        QHash<QString, QString> values;
        for (const QString &line : lines) {
            int idx = line.indexOf('=');
            if (idx <= 0) {
                continue;
            }
            QString key = line.left(idx).trimmed();
            QString value = line.mid(idx + 1).trimmed();
            values.insert(key, value);
        }

        bool ok = false;
        state.power_unit = values.value("POWER_UNIT").toInt(&ok);
        if (!ok) {
            if (err) {
                *err = "Missing POWER_UNIT from helper.";
            }
            return false;
        }

        state.unit_watts = values.value("UNIT_WATTS").toDouble(&ok);
        if (!ok) {
            if (err) {
                *err = "Missing UNIT_WATTS from helper.";
            }
            return false;
        }

        state.msr = values.value("MSR").toULongLong(&ok, 0);
        if (!ok) {
            if (err) {
                *err = "Missing MSR value from helper.";
            }
            return false;
        }

        state.mmio = values.value("MMIO").toULongLong(&ok, 0);
        if (!ok) {
            if (err) {
                *err = "Missing MMIO value from helper.";
            }
            return false;
        }

        state.core_type_supported = values.value("CORE_TYPE_SUPPORTED").toInt(&ok) == 1;
        state.p_cpus = values.value("P_CPUS");
        state.e_cpus = values.value("E_CPUS");
        state.u_cpus = values.value("U_CPUS");
        state.p_ratio_valid = values.value("P_RATIO_VALID").toInt(&ok) == 1;
        state.e_ratio_valid = values.value("E_RATIO_VALID").toInt(&ok) == 1;
        state.p_ratio_cur_valid = values.value("P_RATIO_CUR_VALID").toInt(&ok) == 1;
        state.e_ratio_cur_valid = values.value("E_RATIO_CUR_VALID").toInt(&ok) == 1;

        int ratio = values.value("P_RATIO_TARGET").toInt(&ok);
        state.p_ratio = ok ? ratio : 0;
        ratio = values.value("E_RATIO_TARGET").toInt(&ok);
        state.e_ratio = ok ? ratio : 0;

        ratio = values.value("P_RATIO_CUR").toInt(&ok);
        state.p_ratio_cur = ok ? ratio : 0;
        ratio = values.value("E_RATIO_CUR").toInt(&ok);
        state.e_ratio_cur = ok ? ratio : 0;

        state.core_uv_valid = values.value("CORE_UV_VALID").toInt(&ok) == 1;
        state.core_uv_mv = values.value("CORE_UV_MV").toDouble(&ok);
        state.core_uv_raw = values.value("CORE_UV_RAW");

        return true;
    }

    QString helper_path_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow() {
        setWindowTitle("Limits UI");

        auto *central = new QWidget();
        central_ = central;
        auto *main_layout = new QVBoxLayout();
        const int spacing = std::max(6, fontMetrics().height() / 2);
        main_layout->setContentsMargins(spacing, spacing, spacing, spacing);
        main_layout->setSpacing(spacing);

        auto *title = new QLabel("Limits UI (MSR 0x610 + MCHBAR 0x59A0)");
        QFont title_font = title->font();
        title_font.setPointSize(title_font.pointSize() + 2);
        title_font.setBold(true);
        title->setFont(title_font);
        main_layout->addWidget(title);

        cpu_group_ = new QGroupBox();
        cpu_group_->setFlat(true);
        cpu_grid_ = new QGridLayout();
        cpu_grid_->setVerticalSpacing(spacing);
        cpu_grid_->setHorizontalSpacing(spacing);

        cpu_vendor_ = new QLabel("-");
        cpu_model_name_ = new QLabel("-");
        cpu_model_name_->setWordWrap(true);
        cpu_family_model_ = new QLabel("-");
        cpu_family_model_->setWordWrap(true);
        cpu_microcode_ = new QLabel("-");
        cpu_cache_ = new QLabel("-");
        cpu_logical_ = new QLabel("-");
        cpu_physical_ = new QLabel("-");
        cpu_packages_ = new QLabel("-");
        cpu_freq_ = new QLabel("-");
        cpu_p_count_ = new QLabel("-");
        cpu_e_count_ = new QLabel("-");
        cpu_p_mhz_ = new QLabel("-");
        cpu_e_mhz_ = new QLabel("-");

        auto add_cpu_row = [&](const QString &label_text, QWidget *value) {
            QLabel *label = new QLabel(label_text);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            cpu_rows_.push_back({label, value});
        };

        add_cpu_row("Vendor", cpu_vendor_);
        add_cpu_row("Model", cpu_model_name_);
        add_cpu_row("Family/Model/Stepping", cpu_family_model_);
        add_cpu_row("Microcode", cpu_microcode_);
        add_cpu_row("Cache", cpu_cache_);
        add_cpu_row("Logical CPUs", cpu_logical_);
        add_cpu_row("Physical cores", cpu_physical_);
        add_cpu_row("Packages", cpu_packages_);
        add_cpu_row("Min/Max MHz", cpu_freq_);
        add_cpu_row("P cores (detected)", cpu_p_count_);
        add_cpu_row("E cores (detected)", cpu_e_count_);
        add_cpu_row("P cores MHz", cpu_p_mhz_);
        add_cpu_row("E cores MHz", cpu_e_mhz_);

        layout_grid_rows(cpu_grid_, cpu_rows_, false);
        cpu_group_->setLayout(cpu_grid_);
        cpu_section_ = new CollapsibleSection("CPU info", cpu_group_, spacing);

        status_group_ = new QGroupBox();
        status_group_->setFlat(true);
        status_grid_ = new QGridLayout();
        status_grid_->setVerticalSpacing(spacing);
        status_grid_->setHorizontalSpacing(spacing);

        unit_label_ = new QLabel("unknown");

        msr_raw_ = make_readonly_line();
        mmio_raw_ = make_readonly_line();

        msr_pl1_ = new QLabel("-");
        msr_pl2_ = new QLabel("-");
        mmio_pl1_ = new QLabel("-");
        mmio_pl2_ = new QLabel("-");
        p_cpus_ = new QLabel("-");
        e_cpus_ = new QLabel("-");
        u_cpus_ = new QLabel("-");

        auto add_status_row = [&](const QString &label_text, QWidget *value) {
            QLabel *label = new QLabel(label_text);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            status_rows_.push_back({label, value});
        };

        add_status_row("Power unit", unit_label_);
        add_status_row("MSR raw", msr_raw_);
        add_status_row("MSR PL1", msr_pl1_);
        add_status_row("MSR PL2", msr_pl2_);
        add_status_row("MMIO raw", mmio_raw_);
        add_status_row("MMIO PL1", mmio_pl1_);
        add_status_row("MMIO PL2", mmio_pl2_);
        add_status_row("P cores", p_cpus_);
        add_status_row("E cores", e_cpus_);
        add_status_row("Unknown cores", u_cpus_);

        layout_grid_rows(status_grid_, status_rows_, false);
        status_group_->setLayout(status_grid_);
        status_section_ = new CollapsibleSection("Status", status_group_, spacing);

        top_row_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
        top_row_layout_->setSpacing(spacing);
        top_row_layout_->addWidget(cpu_section_);
        top_row_layout_->addWidget(status_section_);
        main_layout->addLayout(top_row_layout_);

        set_group_ = new QGroupBox();
        set_group_->setFlat(true);
        auto *set_layout = new QVBoxLayout();
        set_layout->setSpacing(spacing);
        auto *set_form = new QFormLayout();
        set_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        set_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        set_form->setVerticalSpacing(spacing);
        set_form->setHorizontalSpacing(spacing);

        pl1_spin_ = new QDoubleSpinBox();
        pl1_spin_->setRange(1.0, 5000.0);
        pl1_spin_->setDecimals(2);
        pl1_spin_->setSingleStep(1.0);

        pl2_spin_ = new QDoubleSpinBox();
        pl2_spin_->setRange(1.0, 5000.0);
        pl2_spin_->setDecimals(2);
        pl2_spin_->setSingleStep(1.0);

        set_form->addRow("PL1 (W)", pl1_spin_);
        set_form->addRow("PL2 (W)", pl2_spin_);
        set_layout->addLayout(set_form);

        auto *set_buttons = new QHBoxLayout();
        set_buttons->setSpacing(spacing);
        set_msr_btn_ = new QPushButton("Set MSR");
        set_mmio_btn_ = new QPushButton("Set MMIO");
        set_both_btn_ = new QPushButton("Set Both");

        set_buttons->addWidget(set_msr_btn_);
        set_buttons->addWidget(set_mmio_btn_);
        set_buttons->addWidget(set_both_btn_);
        set_layout->addLayout(set_buttons);

        powercap_check_ = new QCheckBox("Also set kernel powercap (intel-rapl)");
        powercap_check_->setChecked(true);
        set_layout->addWidget(powercap_check_);

        set_group_->setLayout(set_layout);
        set_section_ = new CollapsibleSection("Set limits (watts)", set_group_, spacing);

        ratio_group_ = new QGroupBox();
        ratio_group_->setFlat(true);
        auto *ratio_layout = new QVBoxLayout();
        ratio_layout->setSpacing(spacing);
        ratio_grid_ = new QGridLayout();
        ratio_grid_->setVerticalSpacing(spacing);
        ratio_grid_->setHorizontalSpacing(spacing);

        p_ratio_spin_ = new QSpinBox();
        p_ratio_spin_->setRange(1, 255);
        p_ratio_spin_->setSingleStep(1);

        e_ratio_spin_ = new QSpinBox();
        e_ratio_spin_->setRange(1, 255);
        e_ratio_spin_->setSingleStep(1);

        p_ratio_cur_ = new QLabel("-");
        e_ratio_cur_ = new QLabel("-");

        auto add_ratio_row = [&](const QString &label_text, QWidget *value) {
            QLabel *label = new QLabel(label_text);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            ratio_rows_.push_back({label, value});
        };

        add_ratio_row("P-core ratio target (x)", p_ratio_spin_);
        add_ratio_row("P-core ratio current", p_ratio_cur_);
        add_ratio_row("E-core ratio target (x)", e_ratio_spin_);
        add_ratio_row("E-core ratio current", e_ratio_cur_);
        layout_grid_rows(ratio_grid_, ratio_rows_, false);
        ratio_layout->addLayout(ratio_grid_);

        auto *ratio_buttons = new QHBoxLayout();
        ratio_buttons->setSpacing(spacing);
        set_p_ratio_btn_ = new QPushButton("Set P");
        set_e_ratio_btn_ = new QPushButton("Set E");
        set_pe_ratio_btn_ = new QPushButton("Set P+E");
        set_all_ratio_btn_ = new QPushButton("Set All");

        ratio_buttons->addWidget(set_p_ratio_btn_);
        ratio_buttons->addWidget(set_e_ratio_btn_);
        ratio_buttons->addWidget(set_pe_ratio_btn_);
        ratio_buttons->addWidget(set_all_ratio_btn_);
        ratio_layout->addLayout(ratio_buttons);

        ratio_group_->setLayout(ratio_layout);
        ratio_section_ = new CollapsibleSection("CPU ratio (multiplier)", ratio_group_, spacing);

        uv_group_ = new QGroupBox();
        uv_group_->setFlat(true);
        auto *uv_layout = new QVBoxLayout();
        uv_layout->setSpacing(spacing);
        uv_grid_ = new QGridLayout();
        uv_grid_->setVerticalSpacing(spacing);
        uv_grid_->setHorizontalSpacing(spacing);

        core_uv_spin_ = new QDoubleSpinBox();
        core_uv_spin_->setRange(-500.0, 500.0);
        core_uv_spin_->setDecimals(0);
        core_uv_spin_->setSingleStep(1.0);
        core_uv_spin_->setSuffix(" mV");
        core_uv_spin_->setToolTip("Hardware quantizes to ~0.977 mV steps.");

        core_uv_cur_ = new QLabel("-");
        core_uv_raw_ = new QLabel("-");

        auto add_uv_row = [&](const QString &label_text, QWidget *value) {
            QLabel *label = new QLabel(label_text);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            uv_rows_.push_back({label, value});
        };

        add_uv_row("Core offset target (mV)", core_uv_spin_);
        add_uv_row("Core offset current", core_uv_cur_);
        add_uv_row("Core offset raw", core_uv_raw_);
        layout_grid_rows(uv_grid_, uv_rows_, false);
        uv_layout->addLayout(uv_grid_);

        core_uv_btn_ = new QPushButton("Set Core Offset");
        uv_layout->addWidget(core_uv_btn_);

        uv_group_->setLayout(uv_layout);
        uv_section_ = new CollapsibleSection("Voltage offset (mV)", uv_group_, spacing);

        ratio_uv_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
        ratio_uv_layout_->setSpacing(spacing);
        ratio_uv_layout_->addWidget(uv_section_);
        ratio_uv_layout_->addWidget(ratio_section_);
        ratio_uv_container_ = new QWidget();
        ratio_uv_container_->setLayout(ratio_uv_layout_);

        mid_row_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
        mid_row_layout_->setSpacing(spacing);
        mid_row_layout_->addWidget(set_section_);
        mid_row_layout_->addWidget(ratio_uv_container_);
        main_layout->addLayout(mid_row_layout_);

        sync_group_ = new QGroupBox();
        sync_group_->setFlat(true);
        auto *sync_layout = new QVBoxLayout();
        sync_layout->setSpacing(spacing);

        sync_buttons_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
        sync_buttons_layout_->setSpacing(spacing);
        refresh_btn_ = new QPushButton("Refresh");
        sync_msr_to_mmio_btn_ = new QPushButton("MSR -> MMIO");
        sync_mmio_to_msr_btn_ = new QPushButton("MMIO -> MSR");
        sync_buttons_layout_->addWidget(refresh_btn_);
        sync_buttons_layout_->addWidget(sync_msr_to_mmio_btn_);
        sync_buttons_layout_->addWidget(sync_mmio_to_msr_btn_);
        sync_layout->addLayout(sync_buttons_layout_);

        sync_group_->setLayout(sync_layout);
        sync_section_ = new CollapsibleSection("Sync + refresh", sync_group_, spacing);
        main_layout->addWidget(sync_section_);

        services_group_ = new QGroupBox();
        services_group_->setFlat(true);
        auto *services_layout = new QVBoxLayout();
        services_layout->setSpacing(spacing);

        auto make_service_controls = [&](const QString &title,
                                         QPushButton *&start_btn,
                                         QPushButton *&stop_btn,
                                         QPushButton *&enable_btn,
                                         QPushButton *&disable_btn) -> QGroupBox * {
            auto *group = new QGroupBox(title);
            auto *grid = new QGridLayout();
            grid->setVerticalSpacing(spacing / 2);
            grid->setHorizontalSpacing(spacing / 2);
            start_btn = new QPushButton("Start");
            stop_btn = new QPushButton("Stop");
            enable_btn = new QPushButton("Enable");
            disable_btn = new QPushButton("Disable");
            grid->addWidget(start_btn, 0, 0);
            grid->addWidget(stop_btn, 0, 1);
            grid->addWidget(enable_btn, 1, 0);
            grid->addWidget(disable_btn, 1, 1);
            group->setLayout(grid);
            return group;
        };

        thermald_controls_ = make_service_controls("thermald", start_thermald_btn_, stop_thermald_btn_,
                                                   enable_thermald_btn_, disable_thermald_btn_);
        tuned_controls_ = make_service_controls("tuned", start_tuned_btn_, stop_tuned_btn_,
                                                enable_tuned_btn_, disable_tuned_btn_);
        tuned_ppd_controls_ = make_service_controls("tuned-ppd", start_tuned_ppd_btn_, stop_tuned_ppd_btn_,
                                                    enable_tuned_ppd_btn_, disable_tuned_ppd_btn_);

        service_controls_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
        service_controls_layout_->setSpacing(spacing);
        service_controls_layout_->addWidget(thermald_controls_);
        service_controls_layout_->addWidget(tuned_controls_);
        service_controls_layout_->addWidget(tuned_ppd_controls_);
        services_layout->addLayout(service_controls_layout_);
        services_group_->setLayout(services_layout);
        services_section_ = new CollapsibleSection("Services", services_group_, spacing);
        main_layout->addWidget(services_section_);

        profile_group_ = new QGroupBox();
        profile_group_->setFlat(true);
        auto *profile_layout = new QVBoxLayout();
        profile_layout->setSpacing(spacing);

        profile_grid_ = new QGridLayout();
        profile_grid_->setVerticalSpacing(spacing);
        profile_grid_->setHorizontalSpacing(spacing);

        profile_path_ = new QLineEdit();
        profile_browse_btn_ = new QPushButton("Browse");
        auto *profile_row = new QHBoxLayout();
        profile_row->setSpacing(spacing);
        profile_row->addWidget(profile_path_, 1);
        profile_row->addWidget(profile_browse_btn_);
        auto *profile_row_widget = new QWidget();
        profile_row_widget->setLayout(profile_row);
        auto add_profile_row = [&](const QString &label_text, QWidget *value) {
            QLabel *label = new QLabel(label_text);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            profile_rows_.push_back({label, value});
        };

        add_profile_row("Profile file", profile_row_widget);

        fallback_path_ = new QLineEdit();
        fallback_browse_btn_ = new QPushButton("Browse");
        auto *fallback_row = new QHBoxLayout();
        fallback_row->setSpacing(spacing);
        fallback_row->addWidget(fallback_path_, 1);
        fallback_row->addWidget(fallback_browse_btn_);
        auto *fallback_row_widget = new QWidget();
        fallback_row_widget->setLayout(fallback_row);
        add_profile_row("Fallback file", fallback_row_widget);

        layout_grid_rows(profile_grid_, profile_rows_, false);
        profile_layout->addLayout(profile_grid_);

        auto *profile_buttons = new QHBoxLayout();
        profile_buttons->setSpacing(spacing);
        load_profile_btn_ = new QPushButton("Load Profile");
        save_profile_btn_ = new QPushButton("Save Profile");
        profile_buttons->addWidget(load_profile_btn_);
        profile_buttons->addWidget(save_profile_btn_);
        profile_layout->addLayout(profile_buttons);

        startup_grid_ = new QGridLayout();
        startup_grid_->setVerticalSpacing(spacing);
        startup_grid_->setHorizontalSpacing(spacing);

        startup_enabled_ = new QCheckBox("Apply on startup");
        startup_use_fallback_ = new QCheckBox("Use fallback if last startup crashed");
        startup_apply_limits_ = new QCheckBox("Apply PL1/PL2");
        startup_limits_target_ = new QComboBox();
        startup_limits_target_->addItems({"MSR", "MMIO", "Both"});
        startup_apply_ratios_ = new QCheckBox("Apply ratios");
        startup_ratio_target_ = new QComboBox();
        startup_ratio_target_->addItems({"P", "E", "P+E", "All"});
        startup_apply_core_uv_ = new QCheckBox("Apply core UV");

        auto *limits_row = new QHBoxLayout();
        limits_row->setSpacing(spacing);
        limits_row->addWidget(startup_apply_limits_);
        limits_row->addWidget(startup_limits_target_);
        auto *limits_row_widget = new QWidget();
        limits_row_widget->setLayout(limits_row);

        auto *ratio_row = new QHBoxLayout();
        ratio_row->setSpacing(spacing);
        ratio_row->addWidget(startup_apply_ratios_);
        ratio_row->addWidget(startup_ratio_target_);
        auto *ratio_row_widget = new QWidget();
        ratio_row_widget->setLayout(ratio_row);

        auto add_startup_row = [&](const QString &label_text, QWidget *value) {
            QLabel *label = new QLabel(label_text);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            startup_rows_.push_back({label, value});
        };

        add_startup_row("Auto-apply", startup_enabled_);
        add_startup_row("Crash fallback", startup_use_fallback_);
        add_startup_row("Limits", limits_row_widget);
        add_startup_row("Ratios", ratio_row_widget);
        add_startup_row("Core UV", startup_apply_core_uv_);

        layout_grid_rows(startup_grid_, startup_rows_, false);
        profile_layout->addLayout(startup_grid_);

        profile_group_->setLayout(profile_layout);
        profile_section_ = new CollapsibleSection("Profiles + startup", profile_group_, spacing);
        main_layout->addWidget(profile_section_);

        log_ = new QPlainTextEdit();
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(200);
        log_->setMinimumHeight(fontMetrics().height() * 6);
        auto *log_container = new QWidget();
        auto *log_layout = new QVBoxLayout();
        log_layout->setContentsMargins(0, 0, 0, 0);
        log_layout->addWidget(log_);
        log_container->setLayout(log_layout);
        log_section_ = new CollapsibleSection("Log", log_container, spacing);
        main_layout->addWidget(log_section_, 1);

        // Default collapsed state: keep CPU/Status open, collapse the rest.
        set_section_->setExpanded(false);
        uv_section_->setExpanded(false);
        ratio_section_->setExpanded(false);
        sync_section_->setExpanded(false);
        services_section_->setExpanded(false);
        profile_section_->setExpanded(false);
        log_section_->setExpanded(false);

        central->setLayout(main_layout);
        scroll_area_ = new QScrollArea();
        scroll_area_->setWidgetResizable(true);
        scroll_area_->setFrameShape(QFrame::NoFrame);
        scroll_area_->setWidget(central);
        setCentralWidget(scroll_area_);
        central->layout()->activate();
        const QSize hint = central->sizeHint();
        resize(std::clamp(hint.width(), 980, 1600), std::clamp(hint.height(), 720, 1200));
        base_font_ = font();
        base_height_ = height();
        base_width_ = width();
        update_font_scale();
        update_minimum_size();

        connect(refresh_btn_, &QPushButton::clicked, this, &MainWindow::refresh);
        connect(set_msr_btn_, &QPushButton::clicked, this, [this]() { apply_limits(Target::Msr); });
        connect(set_mmio_btn_, &QPushButton::clicked, this, [this]() { apply_limits(Target::Mmio); });
        connect(set_both_btn_, &QPushButton::clicked, this, [this]() { apply_limits(Target::Both); });
        connect(set_p_ratio_btn_, &QPushButton::clicked, this, [this]() { apply_ratio(RatioTarget::P); });
        connect(set_e_ratio_btn_, &QPushButton::clicked, this, [this]() { apply_ratio(RatioTarget::E); });
        connect(set_pe_ratio_btn_, &QPushButton::clicked, this, [this]() { apply_ratio(RatioTarget::Both); });
        connect(set_all_ratio_btn_, &QPushButton::clicked, this, [this]() { apply_ratio(RatioTarget::All); });
        connect(core_uv_btn_, &QPushButton::clicked, this, &MainWindow::apply_core_uv);
        connect(sync_msr_to_mmio_btn_, &QPushButton::clicked, this, &MainWindow::sync_msr_to_mmio);
        connect(sync_mmio_to_msr_btn_, &QPushButton::clicked, this, &MainWindow::sync_mmio_to_msr);
        connect(start_thermald_btn_, &QPushButton::clicked, this, &MainWindow::start_thermald);
        connect(stop_thermald_btn_, &QPushButton::clicked, this, &MainWindow::stop_thermald);
        connect(disable_thermald_btn_, &QPushButton::clicked, this, &MainWindow::disable_thermald);
        connect(enable_thermald_btn_, &QPushButton::clicked, this, &MainWindow::enable_thermald);
        connect(start_tuned_btn_, &QPushButton::clicked, this, &MainWindow::start_tuned);
        connect(stop_tuned_btn_, &QPushButton::clicked, this, &MainWindow::stop_tuned);
        connect(disable_tuned_btn_, &QPushButton::clicked, this, &MainWindow::disable_tuned);
        connect(enable_tuned_btn_, &QPushButton::clicked, this, &MainWindow::enable_tuned);
        connect(start_tuned_ppd_btn_, &QPushButton::clicked, this, &MainWindow::start_tuned_ppd);
        connect(stop_tuned_ppd_btn_, &QPushButton::clicked, this, &MainWindow::stop_tuned_ppd);
        connect(disable_tuned_ppd_btn_, &QPushButton::clicked, this, &MainWindow::disable_tuned_ppd);
        connect(enable_tuned_ppd_btn_, &QPushButton::clicked, this, &MainWindow::enable_tuned_ppd);
        connect(load_profile_btn_, &QPushButton::clicked, this, &MainWindow::load_profile_from_disk);
        connect(save_profile_btn_, &QPushButton::clicked, this, &MainWindow::save_profile_to_disk);
        connect(profile_browse_btn_, &QPushButton::clicked, this, &MainWindow::browse_profile_path);
        connect(fallback_browse_btn_, &QPushButton::clicked, this, &MainWindow::browse_fallback_path);

        connect(profile_path_, &QLineEdit::textChanged, this, &MainWindow::save_preferences);
        connect(fallback_path_, &QLineEdit::textChanged, this, &MainWindow::save_preferences);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        connect(startup_enabled_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
        connect(startup_use_fallback_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_limits_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_ratios_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_core_uv_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
        connect(powercap_check_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
#else
        connect(startup_enabled_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_use_fallback_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_limits_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_ratios_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_core_uv_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(powercap_check_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
#endif
        connect(startup_limits_target_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::save_preferences);
        connect(startup_ratio_target_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::save_preferences);

        connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::on_about_to_quit);

        auto hook_section = [&](CollapsibleSection *section) {
            if (!section) {
                return;
            }
            auto button = section->toggleButton();
            if (!button) {
                return;
            }
            connect(button, &QToolButton::toggled, this, [this](bool) {
                update_responsive_layout();
            });
        };
        hook_section(cpu_section_);
        hook_section(status_section_);
        hook_section(set_section_);
        hook_section(uv_section_);
        hook_section(ratio_section_);
        hook_section(sync_section_);
        hook_section(services_section_);
        hook_section(profile_section_);
        hook_section(log_section_);

        load_cpu_info();
        load_preferences();
        initialize_backend();
        handle_startup_apply();
        update_responsive_layout();
    }

private:
    enum class Target {
        Msr,
        Mmio,
        Both
    };

    enum class RatioTarget {
        P,
        E,
        Both,
        All
    };

    void initialize_backend() {
        QString err;
        if (!backend_.helper_available(&err)) {
            QMessageBox::critical(this, "Helper missing", err);
            set_controls_enabled(false);
            backend_ready_ = false;
            return;
        }
        set_controls_enabled(true);
        backend_ready_ = true;
        refresh();
    }

    struct Profile {
        double pl1_w = 0.0;
        double pl2_w = 0.0;
        int p_ratio = 0;
        int e_ratio = 0;
        double core_uv_mv = 0.0;
    };

    struct Row {
        QLabel *label = nullptr;
        QWidget *value = nullptr;
    };

    void set_controls_enabled(bool enabled) {
        status_group_->setEnabled(enabled);
        set_msr_btn_->setEnabled(enabled);
        set_mmio_btn_->setEnabled(enabled);
        set_both_btn_->setEnabled(enabled);
        set_p_ratio_btn_->setEnabled(enabled);
        set_e_ratio_btn_->setEnabled(enabled);
        set_pe_ratio_btn_->setEnabled(enabled);
        set_all_ratio_btn_->setEnabled(enabled);
        refresh_btn_->setEnabled(enabled);
        sync_msr_to_mmio_btn_->setEnabled(enabled);
        sync_mmio_to_msr_btn_->setEnabled(enabled);
        start_thermald_btn_->setEnabled(enabled);
        stop_thermald_btn_->setEnabled(enabled);
        disable_thermald_btn_->setEnabled(enabled);
        enable_thermald_btn_->setEnabled(enabled);
        start_tuned_btn_->setEnabled(enabled);
        stop_tuned_btn_->setEnabled(enabled);
        disable_tuned_btn_->setEnabled(enabled);
        enable_tuned_btn_->setEnabled(enabled);
        start_tuned_ppd_btn_->setEnabled(enabled);
        stop_tuned_ppd_btn_->setEnabled(enabled);
        disable_tuned_ppd_btn_->setEnabled(enabled);
        enable_tuned_ppd_btn_->setEnabled(enabled);
        pl1_spin_->setEnabled(enabled);
        pl2_spin_->setEnabled(enabled);
        powercap_check_->setEnabled(enabled);
        p_ratio_spin_->setEnabled(enabled);
        e_ratio_spin_->setEnabled(enabled);
        core_uv_spin_->setEnabled(enabled);
        core_uv_btn_->setEnabled(enabled);
    }

    void update_responsive_layout() {
        int w = width();
        int h = height();
        const int short_height = 820;
        const int very_short_height = 720;
        const int very_narrow = 700;

        int top_min = row_min_width({cpu_section_, status_section_}, top_row_layout_);
        int mid_min = row_min_width({set_section_, ratio_uv_container_}, mid_row_layout_);
        int ratio_min = row_min_width({uv_section_, ratio_section_}, ratio_uv_layout_);
        int sync_min = row_min_width({refresh_btn_, sync_msr_to_mmio_btn_, sync_mmio_to_msr_btn_}, sync_buttons_layout_);
        int services_min = row_min_width({thermald_controls_, tuned_controls_, tuned_ppd_controls_}, service_controls_layout_);

        auto choose_dir = [&](int stack_width, int min_width) {
            if (w < very_narrow) {
                return QBoxLayout::TopToBottom;
            }
            if (h < short_height && w >= min_width) {
                return QBoxLayout::LeftToRight;
            }
            return (w < std::max(stack_width, min_width)) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
        };

        top_row_layout_->setDirection(choose_dir(900, top_min));
        ratio_uv_layout_->setDirection(choose_dir(800, ratio_min));
        mid_row_layout_->setDirection(choose_dir(1100, mid_min));
        sync_buttons_layout_->setDirection(choose_dir(760, sync_min));
        service_controls_layout_->setDirection(choose_dir(1100, services_min));

        bool compact = h < short_height;
        int cpu_w = w;
        int status_w = w;
        if (top_row_layout_->direction() == QBoxLayout::LeftToRight) {
            int spacing = top_row_layout_->spacing();
            int content = std::max(0, w - spacing);
            cpu_w = content / 2;
            status_w = content - cpu_w;
        }
        auto should_two_col = [&](int avail_w, int min_w) {
            if (!compact) {
                return false;
            }
            if (avail_w >= min_w) {
                return true;
            }
            if (h < very_short_height && avail_w >= static_cast<int>(min_w * 0.75)) {
                return true;
            }
            return false;
        };

        bool cpu_two_col = should_two_col(cpu_w, grid_two_col_min_width(cpu_rows_, cpu_grid_));
        bool status_two_col = should_two_col(status_w, grid_two_col_min_width(status_rows_, status_grid_));
        layout_grid_rows(cpu_grid_, cpu_rows_, cpu_two_col);
        layout_grid_rows(status_grid_, status_rows_, status_two_col);

        int uv_w = uv_section_ ? uv_section_->width() : w;
        int ratio_w = ratio_section_ ? ratio_section_->width() : w;
        bool uv_two_col = should_two_col(uv_w, grid_two_col_min_width(uv_rows_, uv_grid_));
        bool ratio_two_col = should_two_col(ratio_w, grid_two_col_min_width(ratio_rows_, ratio_grid_));
        layout_grid_rows(uv_grid_, uv_rows_, uv_two_col);
        layout_grid_rows(ratio_grid_, ratio_rows_, ratio_two_col);

        int profile_w = profile_section_ ? profile_section_->width() : w;
        bool profile_two_col = should_two_col(profile_w, grid_two_col_min_width(profile_rows_, profile_grid_));
        layout_grid_rows(profile_grid_, profile_rows_, profile_two_col);

        bool startup_two_col = should_two_col(profile_w, grid_two_col_min_width(startup_rows_, startup_grid_));
        layout_grid_rows(startup_grid_, startup_rows_, startup_two_col);

        update_minimum_size();
    }

    QString config_dir() const {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (dir.isEmpty()) {
            dir = QDir::homePath() + "/.config/limits_ui_qt";
        }
        QDir().mkpath(dir);
        return dir;
    }

    QString guard_path() const {
        return QDir(config_dir()).filePath("startup_guard.json");
    }

    bool write_startup_guard(const QString &profile_path, QString *err) {
        QJsonObject obj;
        obj["profile_path"] = profile_path;
        obj["started_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QJsonDocument doc(obj);

        QSaveFile file(guard_path());
        if (!file.open(QIODevice::WriteOnly)) {
            if (err) {
                *err = QString("Failed to write guard file: %1").arg(file.errorString());
            }
            return false;
        }
        file.write(doc.toJson(QJsonDocument::Compact));
        if (!file.commit()) {
            if (err) {
                *err = QString("Failed to commit guard file: %1").arg(file.errorString());
            }
            return false;
        }
        startup_guard_set_ = true;
        return true;
    }

    bool read_startup_guard(QString *profile_path, QDateTime *started_at, QString *err) const {
        QFile file(guard_path());
        if (!file.exists()) {
            return false;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (err) {
                *err = QString("Failed to open guard file: %1").arg(file.errorString());
            }
            return true;
        }
        QJsonParseError parse_err;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_err);
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (profile_path) {
                *profile_path = obj.value("profile_path").toString();
            }
            if (started_at) {
                QString stamp = obj.value("started_at").toString();
                *started_at = QDateTime::fromString(stamp, Qt::ISODate);
            }
        } else if (err) {
            *err = QString("Invalid guard file: %1").arg(parse_err.errorString());
        }
        return true;
    }

    void clear_startup_guard() {
        QFile::remove(guard_path());
        startup_guard_set_ = false;
    }

    Profile profile_from_ui() const {
        Profile p;
        p.pl1_w = pl1_spin_->value();
        p.pl2_w = pl2_spin_->value();
        p.p_ratio = p_ratio_spin_->value();
        p.e_ratio = e_ratio_spin_->value();
        p.core_uv_mv = core_uv_spin_->value();
        return p;
    }

    void apply_profile_to_ui(const Profile &p) {
        pl1_spin_->setValue(std::clamp(p.pl1_w, pl1_spin_->minimum(), pl1_spin_->maximum()));
        pl2_spin_->setValue(std::clamp(p.pl2_w, pl2_spin_->minimum(), pl2_spin_->maximum()));
        p_ratio_spin_->setValue(std::clamp(p.p_ratio, p_ratio_spin_->minimum(), p_ratio_spin_->maximum()));
        e_ratio_spin_->setValue(std::clamp(p.e_ratio, e_ratio_spin_->minimum(), e_ratio_spin_->maximum()));
        core_uv_spin_->setValue(std::clamp(p.core_uv_mv, core_uv_spin_->minimum(), core_uv_spin_->maximum()));
    }

    bool save_profile_file(const QString &path, const Profile &p, QString *err) const {
        if (path.trimmed().isEmpty()) {
            if (err) {
                *err = "Profile path is empty.";
            }
            return false;
        }
        QJsonObject obj;
        obj["version"] = 1;
        obj["pl1_w"] = p.pl1_w;
        obj["pl2_w"] = p.pl2_w;
        obj["p_ratio"] = p.p_ratio;
        obj["e_ratio"] = p.e_ratio;
        obj["core_uv_mv"] = p.core_uv_mv;
        obj["saved_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QJsonDocument doc(obj);

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (err) {
                *err = QString("Failed to open %1: %2").arg(path, file.errorString());
            }
            return false;
        }
        file.write(doc.toJson(QJsonDocument::Indented));
        if (!file.commit()) {
            if (err) {
                *err = QString("Failed to write %1: %2").arg(path, file.errorString());
            }
            return false;
        }
        return true;
    }

    bool load_profile_file(const QString &path, Profile &p, QString *err) const {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (err) {
                *err = QString("Failed to open %1: %2").arg(path, file.errorString());
            }
            return false;
        }
        QJsonParseError parse_err;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_err);
        if (!doc.isObject()) {
            if (err) {
                *err = QString("Invalid profile JSON: %1").arg(parse_err.errorString());
            }
            return false;
        }
        QJsonObject obj = doc.object();
        if (obj.value("version").toInt() != 1) {
            if (err) {
                *err = "Unsupported profile version.";
            }
            return false;
        }

        auto get_double = [&](const char *key, double &out) -> bool {
            QJsonValue v = obj.value(QLatin1String(key));
            if (!v.isDouble()) {
                return false;
            }
            out = v.toDouble();
            return true;
        };

        auto get_int = [&](const char *key, int &out) -> bool {
            QJsonValue v = obj.value(QLatin1String(key));
            if (!v.isDouble()) {
                return false;
            }
            double raw = v.toDouble();
            long long rounded = std::llround(raw);
            if (std::fabs(raw - static_cast<double>(rounded)) > 0.001) {
                return false;
            }
            out = static_cast<int>(rounded);
            return true;
        };

        double pl1_w = 0.0;
        if (!get_double("pl1_w", pl1_w) || pl1_w <= 0.0) {
            if (err) {
                *err = "Invalid or missing pl1_w.";
            }
            return false;
        }
        double pl2_w = 0.0;
        if (!get_double("pl2_w", pl2_w) || pl2_w <= 0.0) {
            if (err) {
                *err = "Invalid or missing pl2_w.";
            }
            return false;
        }
        int p_ratio = 0;
        if (!get_int("p_ratio", p_ratio) || p_ratio <= 0) {
            if (err) {
                *err = "Invalid or missing p_ratio.";
            }
            return false;
        }
        int e_ratio = 0;
        if (!get_int("e_ratio", e_ratio) || e_ratio <= 0) {
            if (err) {
                *err = "Invalid or missing e_ratio.";
            }
            return false;
        }
        double core_uv = 0.0;
        if (!get_double("core_uv_mv", core_uv)) {
            if (err) {
                *err = "Invalid or missing core_uv_mv.";
            }
            return false;
        }

        p.pl1_w = pl1_w;
        p.pl2_w = pl2_w;
        p.p_ratio = p_ratio;
        p.e_ratio = e_ratio;
        p.core_uv_mv = core_uv;
        return true;
    }

    void browse_profile_path() {
        QString start_dir = QFileInfo(profile_path_->text()).absolutePath();
        if (start_dir.isEmpty() || start_dir == ".") {
            start_dir = config_dir();
        }
        QString path = QFileDialog::getOpenFileName(this, "Select profile", start_dir, "Profile (*.json);;All Files (*)");
        if (!path.isEmpty()) {
            profile_path_->setText(path);
        }
    }

    void browse_fallback_path() {
        QString start_dir = QFileInfo(fallback_path_->text()).absolutePath();
        if (start_dir.isEmpty() || start_dir == ".") {
            start_dir = config_dir();
        }
        QString path = QFileDialog::getOpenFileName(this, "Select fallback profile", start_dir, "Profile (*.json);;All Files (*)");
        if (!path.isEmpty()) {
            fallback_path_->setText(path);
        }
    }

    void save_profile_to_disk() {
        QString path = profile_path_->text().trimmed();
        if (path.isEmpty()) {
            QString start_dir = config_dir();
            path = QFileDialog::getSaveFileName(this, "Save profile", start_dir, "Profile (*.json);;All Files (*)");
            if (path.isEmpty()) {
                return;
            }
            profile_path_->setText(path);
        }
        Profile p = profile_from_ui();
        QString err;
        if (!save_profile_file(path, p, &err)) {
            show_error("Save profile failed", err);
            return;
        }
        log_message(QString("Saved profile to %1").arg(path));
    }

    void load_profile_from_disk() {
        QString path = profile_path_->text().trimmed();
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            QString start_dir = config_dir();
            path = QFileDialog::getOpenFileName(this, "Load profile", start_dir, "Profile (*.json);;All Files (*)");
            if (path.isEmpty()) {
                return;
            }
            profile_path_->setText(path);
        }
        Profile p;
        QString err;
        if (!load_profile_file(path, p, &err)) {
            show_error("Load profile failed", err);
            return;
        }
        apply_profile_to_ui(p);
        log_message(QString("Loaded profile from %1").arg(path));
    }

    void load_preferences() {
        loading_prefs_ = true;
        QSettings settings;
        settings.beginGroup("profiles");
        profile_path_->setText(settings.value("profile_path").toString());
        fallback_path_->setText(settings.value("fallback_path").toString());
        settings.endGroup();

        settings.beginGroup("limits");
        powercap_check_->setChecked(settings.value("apply_powercap", true).toBool());
        settings.endGroup();

        settings.beginGroup("startup");
        startup_enabled_->setChecked(settings.value("enabled", false).toBool());
        startup_use_fallback_->setChecked(settings.value("use_fallback", true).toBool());
        startup_apply_limits_->setChecked(settings.value("apply_limits", true).toBool());
        startup_limits_target_->setCurrentIndex(settings.value("limits_target", 2).toInt());
        startup_apply_ratios_->setChecked(settings.value("apply_ratios", false).toBool());
        startup_ratio_target_->setCurrentIndex(settings.value("ratio_target", 2).toInt());
        startup_apply_core_uv_->setChecked(settings.value("apply_core_uv", false).toBool());
        settings.endGroup();
        loading_prefs_ = false;
    }

    void save_preferences() {
        if (loading_prefs_) {
            return;
        }
        QSettings settings;
        settings.beginGroup("profiles");
        settings.setValue("profile_path", profile_path_->text().trimmed());
        settings.setValue("fallback_path", fallback_path_->text().trimmed());
        settings.endGroup();

        settings.beginGroup("limits");
        settings.setValue("apply_powercap", powercap_check_->isChecked());
        settings.endGroup();

        settings.beginGroup("startup");
        settings.setValue("enabled", startup_enabled_->isChecked());
        settings.setValue("use_fallback", startup_use_fallback_->isChecked());
        settings.setValue("apply_limits", startup_apply_limits_->isChecked());
        settings.setValue("limits_target", startup_limits_target_->currentIndex());
        settings.setValue("apply_ratios", startup_apply_ratios_->isChecked());
        settings.setValue("ratio_target", startup_ratio_target_->currentIndex());
        settings.setValue("apply_core_uv", startup_apply_core_uv_->isChecked());
        settings.endGroup();
    }

    Target startup_limits_target_value() const {
        switch (startup_limits_target_->currentIndex()) {
            case 0:
                return Target::Msr;
            case 1:
                return Target::Mmio;
            default:
                return Target::Both;
        }
    }

    RatioTarget startup_ratio_target_value() const {
        switch (startup_ratio_target_->currentIndex()) {
            case 0:
                return RatioTarget::P;
            case 1:
                return RatioTarget::E;
            case 2:
                return RatioTarget::Both;
            default:
                return RatioTarget::All;
        }
    }

    bool apply_limits_internal(Target target, bool confirm, bool do_refresh) {
        QString err;
        ReadState state;
        if (!backend_.read_state(state, &err)) {
            show_error("Read failed", err);
            return false;
        }
        if (!update_units(state)) {
            show_error("Invalid unit", "Power unit is unknown or zero.");
            return false;
        }

        std::uint16_t pl1_units = 0;
        std::uint16_t pl2_units = 0;
        if (!build_units(state.unit_watts, pl1_units, pl2_units)) {
            return false;
        }

        double pl1_w = pl1_spin_->value();
        double pl2_w = pl2_spin_->value();

        if (target == Target::Msr || target == Target::Both) {
            std::uint64_t next = apply_pl_units(state.msr, pl1_units, pl2_units);
            if (confirm) {
                if (!confirm_action("Write MSR?",
                                    QString("MSR (0x%1) new value: %2")
                                        .arg(kMsrPkgPowerLimit, 0, 16)
                                        .arg(hex64(next)))) {
                    return false;
                }
            }
            if (!backend_.write_msr(next, &err)) {
                show_error("Write MSR failed", err);
                return false;
            }
            log_message(QString("Wrote MSR %1").arg(hex64(next)));
        }

        if (target == Target::Mmio || target == Target::Both) {
            std::uint64_t next = apply_pl_units(state.mmio, pl1_units, pl2_units);
            if (confirm) {
                if (!confirm_action("Write MMIO?",
                                    QString("MMIO (0x%1) new value: %2")
                                        .arg(kMchbarPlOffset, 0, 16)
                                        .arg(hex64(next)))) {
                    return false;
                }
            }
            if (!backend_.write_mmio(next, &err)) {
                show_error("Write MMIO failed", err);
                return false;
            }
            log_message(QString("Wrote MMIO %1").arg(hex64(next)));
        }

        if (powercap_check_ && powercap_check_->isChecked()) {
            std::uint64_t pl1_uw = static_cast<std::uint64_t>(std::llround(pl1_w * 1000000.0));
            std::uint64_t pl2_uw = static_cast<std::uint64_t>(std::llround(pl2_w * 1000000.0));
            if (pl1_uw == 0 || pl2_uw == 0) {
                show_error("Invalid values", "Kernel powercap values must be non-zero.");
                return false;
            }
            if (confirm) {
                if (!confirm_action("Write kernel powercap?",
                                    QString("PL1: %1 W (%2 uW)\nPL2: %3 W (%4 uW)")
                                        .arg(pl1_w, 0, 'f', 2)
                                        .arg(pl1_uw)
                                        .arg(pl2_w, 0, 'f', 2)
                                        .arg(pl2_uw))) {
                    return false;
                }
            }
            if (!backend_.write_powercap(pl1_uw, pl2_uw, &err)) {
                show_error("Write powercap failed", err);
                return false;
            }
            log_message(QString("Wrote kernel powercap PL1=%1W PL2=%2W")
                            .arg(pl1_w, 0, 'f', 2)
                            .arg(pl2_w, 0, 'f', 2));
        }

        if (do_refresh) {
            refresh();
        }
        return true;
    }

    bool apply_ratio_internal(RatioTarget target, bool confirm, bool do_refresh) {
        QString err;
        int p_ratio = p_ratio_spin_->value();
        int e_ratio = e_ratio_spin_->value();

        if (target == RatioTarget::P) {
            if (confirm && !confirm_action("Set P-core ratio?",
                                           QString("P-core ratio target: x%1").arg(p_ratio))) {
                return false;
            }
            if (!backend_.set_p_ratio(p_ratio, &err)) {
                show_error("Set P-core ratio failed", err);
                return false;
            }
            log_message(QString("Set P-core ratio x%1").arg(p_ratio));
        } else if (target == RatioTarget::E) {
            if (confirm && !confirm_action("Set E-core ratio?",
                                           QString("E-core ratio target: x%1").arg(e_ratio))) {
                return false;
            }
            if (!backend_.set_e_ratio(e_ratio, &err)) {
                show_error("Set E-core ratio failed", err);
                return false;
            }
            log_message(QString("Set E-core ratio x%1").arg(e_ratio));
        } else if (target == RatioTarget::Both) {
            if (confirm && !confirm_action("Set P/E ratio?",
                                           QString("P-core ratio x%1, E-core ratio x%2")
                                               .arg(p_ratio)
                                               .arg(e_ratio))) {
                return false;
            }
            if (!backend_.set_pe_ratio(p_ratio, e_ratio, &err)) {
                show_error("Set P/E ratio failed", err);
                return false;
            }
            log_message(QString("Set P/E ratio x%1 / x%2").arg(p_ratio).arg(e_ratio));
        } else {
            int ratio = p_ratio;
            if (confirm && !confirm_action("Set all core ratios?",
                                           QString("All cores ratio target: x%1").arg(ratio))) {
                return false;
            }
            if (!backend_.set_all_ratio(ratio, &err)) {
                show_error("Set all ratios failed", err);
                return false;
            }
            log_message(QString("Set all core ratios x%1").arg(ratio));
        }

        if (do_refresh) {
            refresh();
        }
        return true;
    }

    bool apply_core_uv_internal(bool confirm, bool do_refresh) {
        QString err;
        double mv = core_uv_spin_->value();
        double applied = quantize_uv_mv(mv);
        QString detail;
        if (std::fabs(applied - mv) >= 0.0005) {
            detail = QString("Core offset target: %1 mV\nApplied (quantized): %2 mV")
                         .arg(mv, 0, 'f', 0)
                         .arg(applied, 0, 'f', 3);
        } else {
            detail = QString("Core offset target: %1 mV").arg(mv, 0, 'f', 0);
        }
        if (confirm && !confirm_action("Set core voltage offset?", detail)) {
            return false;
        }
        if (!backend_.set_core_uv(mv, &err)) {
            show_error("Set core offset failed", err);
            return false;
        }
        if (std::fabs(applied - mv) >= 0.0005) {
            log_message(QString("Set core offset %1 mV (applied %2 mV)")
                            .arg(mv, 0, 'f', 0)
                            .arg(applied, 0, 'f', 3));
        } else {
            log_message(QString("Set core offset %1 mV").arg(mv, 0, 'f', 0));
        }
        if (do_refresh) {
            refresh();
        }
        return true;
    }

    void handle_startup_apply() {
        if (!backend_ready_) {
            return;
        }
        if (!startup_enabled_->isChecked()) {
            return;
        }

        QString guard_profile;
        QDateTime guard_time;
        QString guard_err;
        if (read_startup_guard(&guard_profile, &guard_time, &guard_err)) {
            QString when = guard_time.isValid() ? guard_time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : "unknown time";
            log_message(QString("Startup guard detected (%1). Skipping auto-apply.").arg(when));
            if (!guard_err.isEmpty()) {
                log_message(QString("Guard warning: %1").arg(guard_err));
            }
            startup_enabled_->setChecked(false);
            save_preferences();
            if (startup_use_fallback_->isChecked() && !fallback_path_->text().trimmed().isEmpty()) {
                Profile fallback;
                QString err;
                if (!load_profile_file(fallback_path_->text().trimmed(), fallback, &err)) {
                    show_error("Fallback profile failed", err);
                    clear_startup_guard();
                    return;
                }
                apply_profile_to_ui(fallback);
                if (!apply_startup_actions()) {
                    clear_startup_guard();
                    return;
                }
                log_message(QString("Applied fallback profile from %1").arg(fallback_path_->text().trimmed()));
            } else {
                show_error("Startup crash detected",
                           "Previous auto-apply did not finish. Auto-apply has been disabled for safety.");
            }
            clear_startup_guard();
            return;
        }

        QString profile_path = profile_path_->text().trimmed();
        if (profile_path.isEmpty()) {
            log_message("Startup enabled but no profile path set.");
            return;
        }

        Profile profile;
        QString err;
        if (!load_profile_file(profile_path, profile, &err)) {
            show_error("Startup profile failed", err);
            return;
        }
        apply_profile_to_ui(profile);

        if (!write_startup_guard(profile_path, &err)) {
            log_message(QString("Warning: %1").arg(err));
        }
        if (!apply_startup_actions()) {
            clear_startup_guard();
            return;
        }
        log_message(QString("Applied startup profile from %1").arg(profile_path));
    }

    bool apply_startup_actions() {
        bool ok = true;
        if (startup_apply_limits_->isChecked()) {
            ok = apply_limits_internal(startup_limits_target_value(), false, false);
        }
        if (ok && startup_apply_ratios_->isChecked()) {
            ok = apply_ratio_internal(startup_ratio_target_value(), false, false);
        }
        if (ok && startup_apply_core_uv_->isChecked()) {
            ok = apply_core_uv_internal(false, false);
        }
        if (ok) {
            refresh();
        }
        return ok;
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        update_font_scale();
        update_responsive_layout();
    }

    QLineEdit *make_readonly_line() {
        auto *line = new QLineEdit();
        line->setReadOnly(true);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        line->setFont(mono);
        return line;
    }

    int row_min_width(const std::initializer_list<QWidget *> &widgets, const QBoxLayout *layout) const {
        int total = 0;
        int count = 0;
        for (QWidget *w : widgets) {
            if (!w) {
                continue;
            }
            if (!w->isVisible()) {
                continue;
            }
            total += w->sizeHint().width();
            count++;
        }
        if (count > 1 && layout) {
            int spacing = layout->spacing();
            if (spacing < 0) {
                spacing = style()->layoutSpacing(QSizePolicy::GroupBox, QSizePolicy::GroupBox, Qt::Horizontal);
            }
            total += spacing * (count - 1);
            QMargins m = layout->contentsMargins();
            total += m.left() + m.right();
        }
        return total;
    }

    void update_font_scale() {
        if (font_updating_) {
            return;
        }
        if (base_height_ <= 0 || base_width_ <= 0) {
            return;
        }
        const int h = height();
        const int w = width();
        double scale_h = static_cast<double>(h) / static_cast<double>(base_height_);
        double scale_w = static_cast<double>(w) / static_cast<double>(base_width_);
        double scale = std::min(scale_h, scale_w);
        scale = std::clamp(scale, kMinFontScale, 1.0);
        if (std::fabs(scale - font_scale_) < 0.02) {
            return;
        }
        font_updating_ = true;
        QFont f = base_font_;
        double size = f.pointSizeF();
        if (size <= 0.0) {
            size = static_cast<double>(f.pointSize());
        }
        if (size > 0.0) {
            f.setPointSizeF(size * scale);
            setFont(f);
            font_scale_ = scale;
        }
        font_updating_ = false;
    }

    void update_minimum_size() {
        if (size_updating_ || !central_) {
            return;
        }
        size_updating_ = true;
        setMinimumSize(780, 520);
        size_updating_ = false;
    }

    void layout_grid_rows(QGridLayout *grid, const std::vector<Row> &rows, bool two_col) {
        if (!grid) {
            return;
        }
        int split = two_col ? static_cast<int>((rows.size() + 1) / 2) : static_cast<int>(rows.size());
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const Row &row = rows[static_cast<size_t>(i)];
            if (!row.label || !row.value) {
                continue;
            }
            int col_group = (two_col && i >= split) ? 1 : 0;
            int row_idx = (two_col && i >= split) ? (i - split) : i;
            int base_col = col_group * 2;
            grid->addWidget(row.label, row_idx, base_col);
            grid->addWidget(row.value, row_idx, base_col + 1);
        }
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, two_col ? 1 : 0);
    }

    int grid_two_col_min_width(const std::vector<Row> &rows, const QGridLayout *grid) const {
        if (!grid || rows.empty()) {
            return 0;
        }
        int split = static_cast<int>((rows.size() + 1) / 2);
        int max_label_left = 0;
        int max_value_left = 0;
        int max_label_right = 0;
        int max_value_right = 0;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const Row &row = rows[static_cast<size_t>(i)];
            if (!row.label || !row.value) {
                continue;
            }
            int label_w = row.label->sizeHint().width();
            int value_w = row.value->sizeHint().width();
            if (i < split) {
                max_label_left = std::max(max_label_left, label_w);
                max_value_left = std::max(max_value_left, value_w);
            } else {
                max_label_right = std::max(max_label_right, label_w);
                max_value_right = std::max(max_value_right, value_w);
            }
        }
        int spacing = grid->horizontalSpacing();
        if (spacing < 0) {
            spacing = style()->layoutSpacing(QSizePolicy::Label, QSizePolicy::Label, Qt::Horizontal);
        }
        QMargins margins = grid->contentsMargins();
        int total = max_label_left + max_value_left + max_label_right + max_value_right;
        total += spacing * 3;
        total += margins.left() + margins.right();
        return total;
    }

    void log_message(const QString &msg) {
        QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        log_->appendPlainText(stamp + "  " + msg);
    }

    void load_cpu_info() {
        CpuInfo info = read_cpu_info();
        cpu_vendor_->setText(info.vendor.isEmpty() ? "-" : info.vendor);
        cpu_model_name_->setText(info.model_name.isEmpty() ? "-" : info.model_name);

        QString fam_model_step;
        if (!info.family.isEmpty() || !info.model.isEmpty() || !info.stepping.isEmpty()) {
            fam_model_step = QString("family %1, model %2, stepping %3")
                                 .arg(info.family.isEmpty() ? "?" : info.family)
                                 .arg(info.model.isEmpty() ? "?" : info.model)
                                 .arg(info.stepping.isEmpty() ? "?" : info.stepping);
        }
        cpu_family_model_->setText(fam_model_step.isEmpty() ? "-" : fam_model_step);

        cpu_microcode_->setText(info.microcode.isEmpty() ? "-" : info.microcode);
        cpu_cache_->setText(info.cache_size.isEmpty() ? "-" : info.cache_size);

        cpu_logical_->setText(info.logical_cpus > 0 ? QString::number(info.logical_cpus) : "-");
        cpu_physical_->setText(info.physical_cores > 0 ? QString::number(info.physical_cores) : "-");
        cpu_packages_->setText(info.packages > 0 ? QString::number(info.packages) : "-");

        if (info.min_mhz > 0.0 || info.max_mhz > 0.0) {
            if (info.min_mhz > 0.0 && info.max_mhz > 0.0) {
                cpu_freq_->setText(QString("%1 / %2")
                                       .arg(info.min_mhz, 0, 'f', 0)
                                       .arg(info.max_mhz, 0, 'f', 0));
            } else if (info.max_mhz > 0.0) {
                cpu_freq_->setText(QString("max %1").arg(info.max_mhz, 0, 'f', 0));
            } else {
                cpu_freq_->setText(QString("min %1").arg(info.min_mhz, 0, 'f', 0));
            }
        } else {
            cpu_freq_->setText("-");
        }
    }

    static int count_list(const QString &list) {
        if (list.isEmpty()) {
            return 0;
        }
        QStringList parts = list.split(',', Qt::SkipEmptyParts);
        return parts.size();
    }

    bool update_units(const ReadState &state) {
        if (state.unit_watts <= 0.0) {
            return false;
        }
        power_unit_ = state.power_unit;
        unit_watts_ = state.unit_watts;
        unit_label_->setText(QString("2^-%1 W = %2 W")
                                 .arg(power_unit_)
                                 .arg(unit_watts_, 0, 'f', 6));
        return true;
    }

    void refresh() {
        QString err;
        ReadState state;
        if (!backend_.read_state(state, &err)) {
            show_error("Read failed", err);
            return;
        }

        if (!update_units(state)) {
            show_error("Invalid unit", "Power unit is unknown or zero.");
            return;
        }

        update_msr(state.msr);
        update_mmio(state.mmio);
        update_core_info(state);
        maybe_init_limits(state);
    }

    void update_msr(std::uint64_t val) {
        msr_raw_->setText(hex64(val));
        std::uint16_t pl1 = static_cast<std::uint16_t>(val & 0x7FFFu);
        std::uint16_t pl2 = static_cast<std::uint16_t>((val >> 32) & 0x7FFFu);
        msr_pl1_->setText(units_to_text(pl1, unit_watts_));
        msr_pl2_->setText(units_to_text(pl2, unit_watts_));
    }

    void update_mmio(std::uint64_t val) {
        mmio_raw_->setText(hex64(val));
        std::uint16_t pl1 = static_cast<std::uint16_t>(val & 0x7FFFu);
        std::uint16_t pl2 = static_cast<std::uint16_t>((val >> 32) & 0x7FFFu);
        mmio_pl1_->setText(units_to_text(pl1, unit_watts_));
        mmio_pl2_->setText(units_to_text(pl2, unit_watts_));
    }

    void update_core_info(const ReadState &state) {
        p_cpus_->setText(state.p_cpus.isEmpty() ? "-" : state.p_cpus);
        e_cpus_->setText(state.e_cpus.isEmpty() ? "-" : state.e_cpus);
        u_cpus_->setText(state.u_cpus.isEmpty() ? "-" : state.u_cpus);

        int p_count = count_list(state.p_cpus);
        int e_count = count_list(state.e_cpus);
        cpu_p_count_->setText(p_count > 0 ? QString::number(p_count) : "-");
        cpu_e_count_->setText(e_count > 0 ? QString::number(e_count) : "-");

        bool has_p = !state.p_cpus.isEmpty();
        bool has_e = !state.e_cpus.isEmpty();
        bool has_any = has_p || has_e || !state.u_cpus.isEmpty();

        p_ratio_spin_->setEnabled(has_p);
        set_p_ratio_btn_->setEnabled(has_p);
        e_ratio_spin_->setEnabled(has_e);
        set_e_ratio_btn_->setEnabled(has_e);
        set_pe_ratio_btn_->setEnabled(has_p || has_e);
        set_all_ratio_btn_->setEnabled(has_any);

        if (state.p_ratio_valid && has_p) {
            p_ratio_spin_->setValue(state.p_ratio);
        }
        if (state.e_ratio_valid && has_e) {
            e_ratio_spin_->setValue(state.e_ratio);
        }

        if (state.p_ratio_cur_valid && has_p) {
            p_ratio_cur_->setText(QString("x%1").arg(state.p_ratio_cur));
        } else {
            p_ratio_cur_->setText("-");
        }
        if (state.e_ratio_cur_valid && has_e) {
            e_ratio_cur_->setText(QString("x%1").arg(state.e_ratio_cur));
        } else {
            e_ratio_cur_->setText("-");
        }

        cpu_p_mhz_->setText(format_mhz_stats(parse_cpu_list(state.p_cpus)));
        cpu_e_mhz_->setText(format_mhz_stats(parse_cpu_list(state.e_cpus)));

        if (state.core_uv_valid) {
            if (!did_init_core_uv_) {
                core_uv_spin_->setValue(state.core_uv_mv);
                did_init_core_uv_ = true;
            }
            core_uv_cur_->setText(QString("%1 mV").arg(state.core_uv_mv, 0, 'f', 0));
        } else {
            core_uv_cur_->setText("-");
        }
        core_uv_raw_->setText(state.core_uv_raw.isEmpty() ? "-" : state.core_uv_raw);
    }

    void maybe_init_limits(const ReadState &state) {
        if (did_init_limits_) {
            return;
        }
        std::uint64_t base = state.msr != 0 ? state.msr : state.mmio;
        std::uint16_t pl1 = static_cast<std::uint16_t>(base & 0x7FFFu);
        std::uint16_t pl2 = static_cast<std::uint16_t>((base >> 32) & 0x7FFFu);
        if (pl1 == 0 || pl2 == 0 || unit_watts_ <= 0.0) {
            return;
        }
        double pl1_w = static_cast<double>(pl1) * unit_watts_;
        double pl2_w = static_cast<double>(pl2) * unit_watts_;
        pl1_spin_->setValue(pl1_w);
        pl2_spin_->setValue(pl2_w);
        did_init_limits_ = true;
    }

    bool build_units(double unit_watts, std::uint16_t &pl1_units, std::uint16_t &pl2_units) {
        if (unit_watts <= 0.0) {
            show_error("Invalid unit", "Power unit is unknown or zero.");
            return false;
        }

        double pl1_w = pl1_spin_->value();
        double pl2_w = pl2_spin_->value();

        std::uint64_t pl1_calc = static_cast<std::uint64_t>(std::llround(pl1_w / unit_watts));
        std::uint64_t pl2_calc = static_cast<std::uint64_t>(std::llround(pl2_w / unit_watts));

        if (pl1_calc == 0 || pl2_calc == 0 || pl1_calc > 0x7FFFu || pl2_calc > 0x7FFFu) {
            show_error("Invalid values", "Converted units out of range.");
            return false;
        }

        pl1_units = static_cast<std::uint16_t>(pl1_calc);
        pl2_units = static_cast<std::uint16_t>(pl2_calc);
        return true;
    }

    void apply_limits(Target target) {
        (void)apply_limits_internal(target, true, true);
    }

    void apply_ratio(RatioTarget target) {
        (void)apply_ratio_internal(target, true, true);
    }

    void apply_core_uv() {
        (void)apply_core_uv_internal(true, true);
    }

    void sync_msr_to_mmio() {
        QString err;
        ReadState state;
        if (!backend_.read_state(state, &err)) {
            show_error("Read failed", err);
            return;
        }

        if (!confirm_action("Sync MSR -> MMIO?",
                            QString("MMIO (0x%1) will be set to %2")
                                .arg(kMchbarPlOffset, 0, 16)
                                .arg(hex64(state.msr)))) {
            return;
        }
        if (!backend_.write_mmio(state.msr, &err)) {
            show_error("Write MMIO failed", err);
            return;
        }
        log_message(QString("Synced MSR -> MMIO (%1)").arg(hex64(state.msr)));
        refresh();
    }

    void sync_mmio_to_msr() {
        QString err;
        ReadState state;
        if (!backend_.read_state(state, &err)) {
            show_error("Read failed", err);
            return;
        }

        if (!confirm_action("Sync MMIO -> MSR?",
                            QString("MSR (0x%1) will be set to %2")
                                .arg(kMsrPkgPowerLimit, 0, 16)
                                .arg(hex64(state.mmio)))) {
            return;
        }
        if (!backend_.write_msr(state.mmio, &err)) {
            show_error("Write MSR failed", err);
            return;
        }
        log_message(QString("Synced MMIO -> MSR (%1)").arg(hex64(state.mmio)));
        refresh();
    }

    void start_thermald() {
        QString detail = "This will start thermald now.";
        if (!confirm_action("Start thermald?", detail)) {
            return;
        }
        QString err;
        if (!backend_.start_thermald(&err)) {
            show_error("Start thermald failed", err);
            return;
        }
        log_message("Started thermald.");
    }

    void stop_thermald() {
        QString detail = "This will stop thermald.";
        if (!confirm_action("Stop thermald?", detail)) {
            return;
        }
        QString err;
        if (!backend_.stop_thermald(&err)) {
            show_error("Stop thermald failed", err);
            return;
        }
        log_message("Stopped thermald.");
    }

    void disable_thermald() {
        QString detail = "This will disable thermald at boot.\n"
                         "Runtime state is unchanged.";
        if (!confirm_action("Disable thermald?", detail)) {
            return;
        }
        QString err;
        if (!backend_.disable_thermald(&err)) {
            show_error("Disable thermald failed", err);
            return;
        }
        log_message("Disabled thermald.");
    }

    void enable_thermald() {
        QString detail = "This will enable thermald at boot.\n"
                         "Runtime state is unchanged.";
        if (!confirm_action("Enable thermald?", detail)) {
            return;
        }
        QString err;
        if (!backend_.enable_thermald(&err)) {
            show_error("Enable thermald failed", err);
            return;
        }
        log_message("Enabled thermald.");
    }

    void start_tuned() {
        QString detail = "This will start tuned now.";
        if (!confirm_action("Start tuned?", detail)) {
            return;
        }
        QString err;
        if (!backend_.start_tuned(&err)) {
            show_error("Start tuned failed", err);
            return;
        }
        log_message("Started tuned.");
    }

    void stop_tuned() {
        QString detail = "This will stop tuned.";
        if (!confirm_action("Stop tuned?", detail)) {
            return;
        }
        QString err;
        if (!backend_.stop_tuned(&err)) {
            show_error("Stop tuned failed", err);
            return;
        }
        log_message("Stopped tuned.");
    }

    void disable_tuned() {
        QString detail = "This will disable tuned at boot.\n"
                         "Runtime state is unchanged.";
        if (!confirm_action("Disable tuned?", detail)) {
            return;
        }
        QString err;
        if (!backend_.disable_tuned(&err)) {
            show_error("Disable tuned failed", err);
            return;
        }
        log_message("Disabled tuned.");
    }

    void enable_tuned() {
        QString detail = "This will enable tuned at boot.\n"
                         "Runtime state is unchanged.";
        if (!confirm_action("Enable tuned?", detail)) {
            return;
        }
        QString err;
        if (!backend_.enable_tuned(&err)) {
            show_error("Enable tuned failed", err);
            return;
        }
        log_message("Enabled tuned.");
    }

    void start_tuned_ppd() {
        QString detail = "This will start tuned-ppd now.";
        if (!confirm_action("Start tuned-ppd?", detail)) {
            return;
        }
        QString err;
        if (!backend_.start_tuned_ppd(&err)) {
            show_error("Start tuned-ppd failed", err);
            return;
        }
        log_message("Started tuned-ppd.");
    }

    void stop_tuned_ppd() {
        QString detail = "This will stop tuned-ppd.";
        if (!confirm_action("Stop tuned-ppd?", detail)) {
            return;
        }
        QString err;
        if (!backend_.stop_tuned_ppd(&err)) {
            show_error("Stop tuned-ppd failed", err);
            return;
        }
        log_message("Stopped tuned-ppd.");
    }

    void disable_tuned_ppd() {
        QString detail = "This will disable tuned-ppd at boot.\n"
                         "Runtime state is unchanged.";
        if (!confirm_action("Disable tuned-ppd?", detail)) {
            return;
        }
        QString err;
        if (!backend_.disable_tuned_ppd(&err)) {
            show_error("Disable tuned-ppd failed", err);
            return;
        }
        log_message("Disabled tuned-ppd.");
    }

    void enable_tuned_ppd() {
        QString detail = "This will enable tuned-ppd at boot.\n"
                         "Runtime state is unchanged.";
        if (!confirm_action("Enable tuned-ppd?", detail)) {
            return;
        }
        QString err;
        if (!backend_.enable_tuned_ppd(&err)) {
            show_error("Enable tuned-ppd failed", err);
            return;
        }
        log_message("Enabled tuned-ppd.");
    }

    bool confirm_action(const QString &title, const QString &detail) {
        QMessageBox box(this);
        box.setWindowTitle(title);
        box.setText(title);
        box.setInformativeText(detail);
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);
        return box.exec() == QMessageBox::Yes;
    }

    void show_error(const QString &title, const QString &detail) {
        QMessageBox::critical(this, title, detail);
        log_message(title + ": " + detail);
    }

    void on_about_to_quit() {
        if (startup_guard_set_) {
            clear_startup_guard();
        }
    }

    HelperBackend backend_;
    int power_unit_ = 0;
    double unit_watts_ = 0.0;
    bool did_init_limits_ = false;
    bool did_init_core_uv_ = false;

    QGroupBox *cpu_group_ = nullptr;
    QGridLayout *cpu_grid_ = nullptr;
    QLabel *cpu_vendor_ = nullptr;
    QLabel *cpu_model_name_ = nullptr;
    QLabel *cpu_family_model_ = nullptr;
    QLabel *cpu_microcode_ = nullptr;
    QLabel *cpu_cache_ = nullptr;
    QLabel *cpu_logical_ = nullptr;
    QLabel *cpu_physical_ = nullptr;
    QLabel *cpu_packages_ = nullptr;
    QLabel *cpu_freq_ = nullptr;
    QLabel *cpu_p_count_ = nullptr;
    QLabel *cpu_e_count_ = nullptr;
    QLabel *cpu_p_mhz_ = nullptr;
    QLabel *cpu_e_mhz_ = nullptr;

    QGroupBox *status_group_ = nullptr;
    QGridLayout *status_grid_ = nullptr;
    QLabel *unit_label_ = nullptr;
    QLineEdit *msr_raw_ = nullptr;
    QLineEdit *mmio_raw_ = nullptr;
    QLabel *msr_pl1_ = nullptr;
    QLabel *msr_pl2_ = nullptr;
    QLabel *mmio_pl1_ = nullptr;
    QLabel *mmio_pl2_ = nullptr;
    QLabel *p_cpus_ = nullptr;
    QLabel *e_cpus_ = nullptr;
    QLabel *u_cpus_ = nullptr;

    QDoubleSpinBox *pl1_spin_ = nullptr;
    QDoubleSpinBox *pl2_spin_ = nullptr;
    QCheckBox *powercap_check_ = nullptr;
    QSpinBox *p_ratio_spin_ = nullptr;
    QSpinBox *e_ratio_spin_ = nullptr;
    QLabel *p_ratio_cur_ = nullptr;
    QLabel *e_ratio_cur_ = nullptr;
    QDoubleSpinBox *core_uv_spin_ = nullptr;
    QLabel *core_uv_cur_ = nullptr;
    QLabel *core_uv_raw_ = nullptr;

    QPushButton *refresh_btn_ = nullptr;
    QPushButton *set_msr_btn_ = nullptr;
    QPushButton *set_mmio_btn_ = nullptr;
    QPushButton *set_both_btn_ = nullptr;
    QPushButton *set_p_ratio_btn_ = nullptr;
    QPushButton *set_e_ratio_btn_ = nullptr;
    QPushButton *set_pe_ratio_btn_ = nullptr;
    QPushButton *set_all_ratio_btn_ = nullptr;
    QPushButton *core_uv_btn_ = nullptr;
    QPushButton *sync_msr_to_mmio_btn_ = nullptr;
    QPushButton *sync_mmio_to_msr_btn_ = nullptr;
    QPushButton *start_thermald_btn_ = nullptr;
    QPushButton *stop_thermald_btn_ = nullptr;
    QPushButton *disable_thermald_btn_ = nullptr;
    QPushButton *enable_thermald_btn_ = nullptr;
    QPushButton *start_tuned_btn_ = nullptr;
    QPushButton *stop_tuned_btn_ = nullptr;
    QPushButton *disable_tuned_btn_ = nullptr;
    QPushButton *enable_tuned_btn_ = nullptr;
    QPushButton *start_tuned_ppd_btn_ = nullptr;
    QPushButton *stop_tuned_ppd_btn_ = nullptr;
    QPushButton *disable_tuned_ppd_btn_ = nullptr;
    QPushButton *enable_tuned_ppd_btn_ = nullptr;

    QLineEdit *profile_path_ = nullptr;
    QPushButton *profile_browse_btn_ = nullptr;
    QPushButton *load_profile_btn_ = nullptr;
    QPushButton *save_profile_btn_ = nullptr;
    QLineEdit *fallback_path_ = nullptr;
    QPushButton *fallback_browse_btn_ = nullptr;
    QCheckBox *startup_enabled_ = nullptr;
    QCheckBox *startup_use_fallback_ = nullptr;
    QCheckBox *startup_apply_limits_ = nullptr;
    QComboBox *startup_limits_target_ = nullptr;
    QCheckBox *startup_apply_ratios_ = nullptr;
    QComboBox *startup_ratio_target_ = nullptr;
    QCheckBox *startup_apply_core_uv_ = nullptr;

    QGroupBox *set_group_ = nullptr;
    CollapsibleSection *cpu_section_ = nullptr;
    CollapsibleSection *status_section_ = nullptr;
    CollapsibleSection *set_section_ = nullptr;
    CollapsibleSection *ratio_section_ = nullptr;
    CollapsibleSection *uv_section_ = nullptr;
    CollapsibleSection *sync_section_ = nullptr;
    CollapsibleSection *services_section_ = nullptr;
    CollapsibleSection *profile_section_ = nullptr;
    CollapsibleSection *log_section_ = nullptr;
    QGroupBox *ratio_group_ = nullptr;
    QGroupBox *uv_group_ = nullptr;
    QWidget *ratio_uv_container_ = nullptr;
    QGroupBox *sync_group_ = nullptr;
    QGroupBox *services_group_ = nullptr;
    QGroupBox *thermald_controls_ = nullptr;
    QGroupBox *tuned_controls_ = nullptr;
    QGroupBox *tuned_ppd_controls_ = nullptr;
    QGroupBox *profile_group_ = nullptr;
    QWidget *central_ = nullptr;
    QScrollArea *scroll_area_ = nullptr;

    QPlainTextEdit *log_ = nullptr;

    QBoxLayout *top_row_layout_ = nullptr;
    QBoxLayout *mid_row_layout_ = nullptr;
    QBoxLayout *ratio_uv_layout_ = nullptr;
    QBoxLayout *sync_buttons_layout_ = nullptr;
    QBoxLayout *service_controls_layout_ = nullptr;

    std::vector<Row> cpu_rows_;
    std::vector<Row> status_rows_;
    QGridLayout *ratio_grid_ = nullptr;
    QGridLayout *uv_grid_ = nullptr;
    std::vector<Row> ratio_rows_;
    std::vector<Row> uv_rows_;
    QGridLayout *profile_grid_ = nullptr;
    QGridLayout *startup_grid_ = nullptr;
    std::vector<Row> profile_rows_;
    std::vector<Row> startup_rows_;

    bool loading_prefs_ = false;
    bool startup_guard_set_ = false;
    bool backend_ready_ = false;
    bool font_updating_ = false;
    bool size_updating_ = false;
    double font_scale_ = 1.0;
    int base_height_ = 0;
    int base_width_ = 0;
    QFont base_font_;
};

#include "main.moc"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("limits_droper");
    QCoreApplication::setApplicationName("limits_ui_qt");
    MainWindow window;
    window.show();
    return app.exec();
}
