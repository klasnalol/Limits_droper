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
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHideEvent>
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
#include <QShowEvent>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSettings>
#include <QSet>
#include <QStyle>
#include <QSize>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QString>
#include <QVBoxLayout>
#include <QtGlobal>
#include <QAction>
#include <QCloseEvent>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSystemTrayIcon>
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

QIcon create_app_icon() {
    QIcon icon;
    const int sizes[] = {16, 22, 24, 32, 48, 64, 128, 256};
    for (int sz : sizes) {
        QPixmap px(sz, sz);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        QRect rect = px.rect().adjusted(1, 1, -1, -1);
        // dark rounded background
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(45, 45, 45));
        int radius = std::max(2, sz / 8);
        p.drawRoundedRect(rect, radius, radius);
        // "TDP" text
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(std::max(6, static_cast<int>(sz * 0.42)));
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(rect, Qt::AlignCenter, "TDP");
        // crossed-out line
        QPen pen(QColor(235, 60, 60));
        pen.setWidthF(std::max(1.0, sz * 0.1));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        int margin = std::max(2, static_cast<int>(sz * 0.12));
        p.drawLine(rect.left() + margin, rect.bottom() - margin,
                   rect.right() - margin, rect.top() + margin);
        p.end();
        icon.addPixmap(px);
    }
    return icon;
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

struct HwmonTemp {
    int index = -1;
    QString label;
    QString input_path;
};

QList<HwmonTemp> discover_coretemp_inputs() {
    QList<HwmonTemp> result;
    QDir hwmon_dir("/sys/class/hwmon");
    for (const QString &name : hwmon_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString name_path = hwmon_dir.absoluteFilePath(name + "/name");
        if (read_text_file(name_path) != "coretemp") {
            continue;
        }
        QString base = hwmon_dir.absoluteFilePath(name);
        QDir base_dir(base);
        for (const QString &entry : base_dir.entryList(QDir::Files)) {
            if (!entry.startsWith("temp") || !entry.endsWith("_input")) {
                continue;
            }
            QString num_part = entry.mid(4, entry.length() - 4 - 6);
            bool ok = false;
            int idx = num_part.toInt(&ok);
            if (!ok) {
                continue;
            }
            HwmonTemp t;
            t.index = idx;
            t.input_path = base + "/" + entry;
            t.label = read_text_file(base + "/temp" + num_part + "_label");
            result.append(t);
        }
    }
    return result;
}

int physical_core_for_cpu(int cpu) {
    QString text = read_text_file(QString("/sys/devices/system/cpu/cpu%1/topology/core_id").arg(cpu));
    if (text.isEmpty()) {
        return -1;
    }
    bool ok = false;
    int id = text.toInt(&ok);
    return ok ? id : -1;
}

int read_temp_millidegrees(const QString &path) {
    QString text = read_text_file(path);
    if (text.isEmpty()) {
        return -1;
    }
    bool ok = false;
    int md = text.toInt(&ok);
    if (!ok || md <= 0) {
        return -1;
    }
    return md;
}

QString thermal_status_summary(std::uint64_t status) {
    QStringList flags;
    if (status & 0x01u) {
        flags.append("THERMAL");
    }
    if (status & 0x04u) {
        flags.append("PROCHOT");
    }
    if (status & 0x20u) {
        flags.append("PWR");
    }
    if (status & 0x40u) {
        flags.append("CUR");
    }
    if (flags.isEmpty()) {
        return "OK";
    }
    return flags.join("+");
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

struct CoreSensor {
    int cpu = -1;
    char type = 'U';
    int ratio = 0;
    bool ratio_valid = false;
    std::uint64_t thermal = 0;
    bool thermal_valid = false;
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

    ~HelperBackend() {
        stop_server();
    }

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
        if (!run_command("READ", &out, err)) {
            return false;
        }
        return parse_state(out, state, err);
    }

    bool write_msr(std::uint64_t val, QString *err) const {
        return run_simple(QString("WRITE-MSR %1").arg(hex64(val)), err);
    }

    bool write_mmio(std::uint64_t val, QString *err) const {
        return run_simple(QString("WRITE-MMIO %1").arg(hex64(val)), err);
    }

    bool write_powercap(std::uint64_t pl1_uw, std::uint64_t pl2_uw, QString *err) const {
        return run_simple(QString("WRITE-POWERCAP %1 %2").arg(pl1_uw).arg(pl2_uw), err);
    }

    bool start_thermald(QString *err) const {
        return run_simple("START-THERMALD", err);
    }

    bool stop_thermald(QString *err) const {
        return run_simple("STOP-THERMALD", err);
    }

    bool disable_thermald(QString *err) const {
        return run_simple("DISABLE-THERMALD", err);
    }

    bool enable_thermald(QString *err) const {
        return run_simple("ENABLE-THERMALD", err);
    }

    bool start_tuned(QString *err) const {
        return run_simple("START-TUNED", err);
    }

    bool stop_tuned(QString *err) const {
        return run_simple("STOP-TUNED", err);
    }

    bool disable_tuned(QString *err) const {
        return run_simple("DISABLE-TUNED", err);
    }

    bool enable_tuned(QString *err) const {
        return run_simple("ENABLE-TUNED", err);
    }

    bool start_tuned_ppd(QString *err) const {
        return run_simple("START-TUNED-PPD", err);
    }

    bool stop_tuned_ppd(QString *err) const {
        return run_simple("STOP-TUNED-PPD", err);
    }

    bool disable_tuned_ppd(QString *err) const {
        return run_simple("DISABLE-TUNED-PPD", err);
    }

    bool enable_tuned_ppd(QString *err) const {
        return run_simple("ENABLE-TUNED-PPD", err);
    }

    bool set_p_ratio(int ratio, QString *err) const {
        return run_simple(QString("SET-P-RATIO %1").arg(ratio), err);
    }

    bool set_e_ratio(int ratio, QString *err) const {
        return run_simple(QString("SET-E-RATIO %1").arg(ratio), err);
    }

    bool set_pe_ratio(int ratio_p, int ratio_e, QString *err) const {
        return run_simple(QString("SET-PE-RATIO %1 %2").arg(ratio_p).arg(ratio_e), err);
    }

    bool set_all_ratio(int ratio, QString *err) const {
        return run_simple(QString("SET-ALL-RATIO %1").arg(ratio), err);
    }

    bool set_core_uv(double mv, QString *err) const {
        return run_simple(QString("SET-CORE-UV %1").arg(mv, 0, 'f', 3), err);
    }

    bool set_cpu_ratio(int cpu, int ratio, QString *err) const {
        return run_simple(QString("SET-CPU-RATIO %1 %2").arg(cpu).arg(ratio), err);
    }

    bool read_core_sensors(QList<CoreSensor> &out, QString *err) const {
        QString out_text;
        if (!run_command("READ-CORE-SENSORS", &out_text, err)) {
            return false;
        }
        return parse_core_sensors(out_text, out, err);
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

    bool ensure_server_running(QString *err) const {
        if (server_ && server_->state() == QProcess::Running) {
            return true;
        }
        if (server_) {
            stop_server();
        }

        server_ = new QProcess();
        server_->setProgram("pkexec");
        QStringList args;
        args << helper_path_ << "--server";
        server_->setArguments(args);
        server_->start();

        if (!server_->waitForStarted(-1)) {
            if (err) {
                *err = "Failed to start helper server.";
            }
            stop_server();
            return false;
        }

        // Give pkexec a moment to prompt and authorize. We don't wait for
        // output here because the server only speaks when commanded.
        if (!server_->waitForReadyRead(100)) {
            // Not an error yet; auth may still be in progress.
        }

        // Consume any initial output (e.g. pkexec messages).
        server_->readAllStandardOutput();
        server_->readAllStandardError();
        return true;
    }

    void stop_server() const {
        if (!server_) {
            return;
        }
        if (server_->state() == QProcess::Running) {
            server_->write("QUIT\n");
            server_->waitForBytesWritten(1000);
            server_->waitForFinished(1000);
        }
        if (server_->state() != QProcess::NotRunning) {
            server_->terminate();
            server_->waitForFinished(1000);
            if (server_->state() != QProcess::NotRunning) {
                server_->kill();
                server_->waitForFinished(1000);
            }
        }
        delete server_;
        server_ = nullptr;
    }

    bool run_simple(const QString &command, QString *err) const {
        QString out;
        if (!run_command(command, &out, err)) {
            return false;
        }
        return true;
    }

    bool run_command(const QString &command, QString *out, QString *err) const {
        if (!ensure_server_running(err)) {
            return false;
        }

        server_->write(command.toUtf8() + "\n");
        if (!server_->waitForBytesWritten(5000)) {
            if (err) {
                *err = "Failed to send command to helper server.";
            }
            stop_server();
            return false;
        }

        QString response;
        QByteArray buffer;
        while (true) {
            if (!server_->waitForReadyRead(10000)) {
                if (err) {
                    *err = "Helper server timed out.";
                }
                stop_server();
                return false;
            }
            buffer.append(server_->readAllStandardOutput());
            response = QString::fromLocal8Bit(buffer);
            if (response.contains("\nEND\n") || response.endsWith("\nEND")) {
                break;
            }
        }

        // Strip the END marker.
        QString text = response.trimmed();
        if (text.endsWith("END")) {
            text.chop(3);
            text = text.trimmed();
        }

        if (out) {
            *out = text;
        }

        // Treat stderr as error if non-empty or if response is empty.
        QString err_text = QString::fromLocal8Bit(server_->readAllStandardError()).trimmed();
        if (!err_text.isEmpty()) {
            if (err) {
                *err = err_text;
            }
            return false;
        }

        if (text.isEmpty()) {
            if (err) {
                *err = "Empty response from helper server.";
            }
            return false;
        }
        return true;
    }

    bool parse_core_sensors(const QString &out, QList<CoreSensor> &sensors, QString *err) const {
        sensors.clear();
        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        int expected = -1;
        QHash<int, CoreSensor> by_index;

        for (const QString &line : lines) {
            if (line.startsWith("CORE_SENSOR_COUNT=")) {
                bool ok = false;
                expected = line.mid(18).toInt(&ok);
                if (!ok) {
                    expected = -1;
                }
                continue;
            }
            if (!line.startsWith("CORE_SENSOR_")) {
                continue;
            }
            QString payload = line.mid(12);
            int eq = payload.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            bool ok = false;
            int idx = payload.left(eq).toInt(&ok);
            if (!ok) {
                continue;
            }
            QStringList parts = payload.mid(eq + 1).split(',', Qt::SkipEmptyParts);
            CoreSensor s;
            for (const QString &part : parts) {
                int sep = part.indexOf('=');
                if (sep <= 0) {
                    continue;
                }
                QString key = part.left(sep).trimmed();
                QString value = part.mid(sep + 1).trimmed();
                if (key == "cpu") {
                    s.cpu = value.toInt(&ok);
                    if (!ok) {
                        s.cpu = -1;
                    }
                } else if (key == "type") {
                    s.type = value.isEmpty() ? 'U' : value.at(0).toLatin1();
                } else if (key == "ratio") {
                    s.ratio = value.toInt(&ok);
                    s.ratio_valid = ok;
                } else if (key == "thermal") {
                    s.thermal = value.toULongLong(&ok, 0);
                    s.thermal_valid = ok;
                }
            }
            by_index.insert(idx, s);
        }

        for (int i = 0; i < std::max(expected, static_cast<int>(by_index.size())); ++i) {
            if (by_index.contains(i)) {
                sensors.append(by_index.value(i));
            }
        }

        if (sensors.isEmpty()) {
            if (err) {
                *err = "No core sensor data from helper.";
            }
            return false;
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
    mutable QProcess *server_ = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow() {
        setWindowTitle("Limits UI");

        QIcon app_icon = QIcon::fromTheme("limits_droper", create_app_icon());
        setWindowIcon(app_icon);
        apply_stylesheet();

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
        close_to_tray_ = new QCheckBox("Close to system tray");
        autostart_enabled_ = new QCheckBox("Start automatically on login");

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
        add_startup_row("Close to tray", close_to_tray_);
        add_startup_row("Autostart", autostart_enabled_);

        layout_grid_rows(startup_grid_, startup_rows_, false);
        profile_layout->addLayout(startup_grid_);

        profile_group_->setLayout(profile_layout);
        profile_section_ = new CollapsibleSection("Profiles + startup", profile_group_, spacing);
        main_layout->addWidget(profile_section_);

        build_per_core_ratio_section(spacing);
        main_layout->addWidget(per_core_section_);

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
        per_core_section_->setExpanded(false);
        log_section_->setExpanded(false);

        central->setLayout(main_layout);
        auto *main_scroll = new QScrollArea();
        main_scroll->setWidgetResizable(true);
        main_scroll->setFrameShape(QFrame::NoFrame);
        main_scroll->setWidget(central);

        build_sensors_tab();

        tab_widget_ = new QTabWidget();
        tab_widget_->addTab(main_scroll, "Main");
        tab_widget_->addTab(sensors_tab_, "Sensors");
        setCentralWidget(tab_widget_);

        central->layout()->activate();
        const QSize hint = central->sizeHint();
        resize(std::clamp(hint.width(), 1040, 1700), std::clamp(hint.height(), 760, 1300));
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
        connect(close_to_tray_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
        connect(powercap_check_, &QCheckBox::checkStateChanged, this, &MainWindow::save_preferences);
#else
        connect(startup_enabled_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_use_fallback_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_limits_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_ratios_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(startup_apply_core_uv_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(close_to_tray_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
        connect(powercap_check_, &QCheckBox::stateChanged, this, &MainWindow::save_preferences);
#endif
        connect(startup_limits_target_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::save_preferences);
        connect(startup_ratio_target_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::save_preferences);
        connect(autostart_enabled_, &QCheckBox::toggled, this, [this]() {
            update_autostart_file();
            save_preferences();
        });

        connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::on_about_to_quit);
        connect(tab_widget_, &QTabWidget::currentChanged, this, [this](int) {
            maybe_start_sensor_timer();
        });

        create_tray_icon(app_icon);

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
        hook_section(per_core_section_);
        hook_section(log_section_);

        load_cpu_info();
        load_preferences();
        initialize_backend();
        handle_startup_apply();
        update_responsive_layout();
    }

private slots:
    void tray_show_hide() {
        if (isVisible()) {
            hide();
        } else {
            show();
            raise();
            activateWindow();
        }
        maybe_start_sensor_timer();
    }

    void tray_quit() {
        quitting_ = true;
        qApp->quit();
    }

private:
    void create_tray_icon(const QIcon &icon) {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            log_message("System tray is not available; close-to-tray disabled.");
            return;
        }
        tray_menu_ = new QMenu(this);
        tray_show_action_ = tray_menu_->addAction("Show / Hide", this, &MainWindow::tray_show_hide);
        tray_menu_->addSeparator();
        tray_quit_action_ = tray_menu_->addAction("Quit", this, &MainWindow::tray_quit);
        tray_icon_ = new QSystemTrayIcon(icon, this);
        tray_icon_->setToolTip("Limits Droper");
        tray_icon_->setContextMenu(tray_menu_);
        connect(tray_icon_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                        tray_show_hide();
                    }
                });
        tray_icon_->show();
    }

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

    struct PerCoreRow {
        int cpu = -1;
        QLabel *type_label = nullptr;
        QLabel *cur_label = nullptr;
        QSpinBox *target_spin = nullptr;
        QPushButton *set_btn = nullptr;
    };

    struct SensorRow {
        int cpu = -1;
        QTableWidgetItem *type_item = nullptr;
        QTableWidgetItem *target_item = nullptr;
        QTableWidgetItem *ratio_item = nullptr;
        QTableWidgetItem *freq_item = nullptr;
        QTableWidgetItem *temp_item = nullptr;
        QTableWidgetItem *throttle_item = nullptr;
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
        if (per_core_apply_all_btn_) {
            per_core_apply_all_btn_->setEnabled(enabled);
        }
        if (per_core_reset_btn_) {
            per_core_reset_btn_->setEnabled(enabled);
        }
        for (const PerCoreRow &row : per_core_rows_) {
            if (row.target_spin) {
                row.target_spin->setEnabled(enabled);
            }
            if (row.set_btn) {
                row.set_btn->setEnabled(enabled);
            }
        }
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
        bool tray_available = QSystemTrayIcon::isSystemTrayAvailable();
        close_to_tray_->setChecked(settings.value("close_to_tray", tray_available).toBool());
        close_to_tray_->setEnabled(tray_available);
        autostart_enabled_->setChecked(settings.value("autostart", false).toBool());
        update_autostart_file();
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
        settings.setValue("close_to_tray", close_to_tray_->isChecked());
        settings.setValue("autostart", autostart_enabled_->isChecked());
        settings.endGroup();
    }

    void update_autostart_file() {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
        if (!QDir().mkpath(dir)) {
            return;
        }
        QString path = QDir(dir).filePath("limits_droper.desktop");
        if (!autostart_enabled_ || !autostart_enabled_->isChecked()) {
            QFile::remove(path);
            return;
        }
        QString exec = QCoreApplication::applicationFilePath();
        QString exec_quoted = exec.contains(QLatin1Char(' ')) ? QString("\"%1\"").arg(exec) : exec;
        QString desktop = QString(
            "[Desktop Entry]\n"
            "Name=Limits Droper\n"
            "Comment=Adjust Intel CPU power limits\n"
            "Exec=%1\n"
            "Icon=limits_droper\n"
            "Type=Application\n"
            "Terminal=false\n"
            "X-GNOME-Autostart-enabled=true\n")
            .arg(exec_quoted);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return;
        }
        file.write(desktop.toUtf8());
        file.commit();
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

    void closeEvent(QCloseEvent *event) override {
        if (quitting_ || !tray_icon_ || !tray_icon_->isVisible() || !close_to_tray_->isChecked()) {
            event->accept();
            return;
        }
        hide();
        event->ignore();
        maybe_start_sensor_timer();
    }

    void showEvent(QShowEvent *event) override {
        QMainWindow::showEvent(event);
        maybe_start_sensor_timer();
    }

    void hideEvent(QHideEvent *event) override {
        QMainWindow::hideEvent(event);
        maybe_start_sensor_timer();
    }

    QLineEdit *make_readonly_line() {
        auto *line = new QLineEdit();
        line->setReadOnly(true);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        line->setFont(mono);
        return line;
    }

    void apply_stylesheet() {
        QString style = R"(
            QMainWindow {
                background-color: #1e1e24;
            }
            QWidget {
                color: #eceff4;
                font-family: "Segoe UI", "Noto Sans", "Ubuntu", sans-serif;
            }
            QTabWidget::pane {
                border: 1px solid #3b4252;
                border-radius: 6px;
                background-color: #24242b;
                padding: 8px;
            }
            QTabBar::tab {
                background-color: #2e3440;
                color: #d8dee9;
                border: 1px solid #3b4252;
                border-bottom: none;
                border-top-left-radius: 6px;
                border-top-right-radius: 6px;
                padding: 8px 18px;
                margin-right: 2px;
            }
            QTabBar::tab:selected {
                background-color: #5e81ac;
                color: #ffffff;
            }
            QTabBar::tab:hover:!selected {
                background-color: #3b4252;
            }
            QGroupBox {
                background-color: #2e3440;
                border: 1px solid #3b4252;
                border-radius: 8px;
                margin-top: 12px;
                padding-top: 10px;
                font-weight: 600;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 6px;
                color: #88c0d0;
            }
            QPushButton {
                background-color: #5e81ac;
                color: #ffffff;
                border: none;
                border-radius: 5px;
                padding: 6px 14px;
                min-width: 70px;
            }
            QPushButton:hover {
                background-color: #81a1c1;
            }
            QPushButton:pressed {
                background-color: #4c566a;
            }
            QPushButton:disabled {
                background-color: #4c566a;
                color: #a0a0a0;
            }
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                background-color: #3b4252;
                border: 1px solid #4c566a;
                border-radius: 4px;
                padding: 4px;
                color: #eceff4;
            }
            QLineEdit:read-only {
                background-color: #2e3440;
            }
            QLabel {
                color: #d8dee9;
            }
            QCheckBox {
                color: #d8dee9;
                spacing: 6px;
            }
            QCheckBox::indicator {
                width: 16px;
                height: 16px;
            }
            QPlainTextEdit {
                background-color: #2e3440;
                border: 1px solid #3b4252;
                border-radius: 6px;
                color: #d8dee9;
                font-family: monospace;
            }
            QTableWidget {
                background-color: #2e3440;
                border: 1px solid #3b4252;
                border-radius: 6px;
                gridline-color: #4c566a;
                color: #d8dee9;
            }
            QTableWidget::item:selected {
                background-color: #5e81ac;
            }
            QHeaderView::section {
                background-color: #3b4252;
                color: #eceff4;
                padding: 6px;
                border: 1px solid #4c566a;
                font-weight: 600;
            }
            QToolButton {
                background-color: transparent;
                border: none;
                color: #88c0d0;
                font-weight: 600;
            }
            QScrollArea {
                border: none;
                background-color: transparent;
            }
        )";
        qApp->setStyleSheet(style);
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

        // Create per-core ratio rows without needing root. Types are refined later by refresh().
        if (!per_core_rows_populated_ && per_core_grid_) {
            int logical = info.logical_cpus > 0 ? info.logical_cpus : QThread::idealThreadCount();
            if (logical <= 0) {
                logical = 1;
            }
            QList<int> cpus;
            for (int i = 0; i < logical; ++i) {
                cpus.append(i);
            }
            populate_per_core_rows(cpus, "?");
            per_core_rows_populated_ = true;
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

        if (!per_core_rows_populated_ && per_core_grid_) {
            // Fallback: if cpuinfo enumeration failed, populate from helper lists.
            QList<int> p_cpus = parse_cpu_list(state.p_cpus);
            QList<int> e_cpus = parse_cpu_list(state.e_cpus);
            QList<int> u_cpus = parse_cpu_list(state.u_cpus);
            int max_cpu = 0;
            for (int cpu : p_cpus + e_cpus + u_cpus) {
                max_cpu = std::max(max_cpu, cpu);
            }
            if (max_cpu > 0) {
                QList<int> all;
                for (int i = 0; i <= max_cpu; ++i) {
                    all.append(i);
                }
                populate_per_core_rows(all, "?");
                per_core_rows_populated_ = true;
            }
        }
        if (per_core_rows_populated_) {
            reset_per_core_ratios();
        }

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

        // Update per-core type labels from the helper's P/E/U lists.
        if (per_core_rows_populated_) {
            QSet<int> p_set;
            QSet<int> e_set;
            QSet<int> u_set;
            for (int cpu : parse_cpu_list(state.p_cpus)) {
                p_set.insert(cpu);
            }
            for (int cpu : parse_cpu_list(state.e_cpus)) {
                e_set.insert(cpu);
            }
            for (int cpu : parse_cpu_list(state.u_cpus)) {
                u_set.insert(cpu);
            }
            for (PerCoreRow &row : per_core_rows_) {
                if (!row.type_label) {
                    continue;
                }
                if (p_set.contains(row.cpu)) {
                    row.type_label->setText("P");
                } else if (e_set.contains(row.cpu)) {
                    row.type_label->setText("E");
                } else if (u_set.contains(row.cpu)) {
                    row.type_label->setText("U");
                }
            }
        }

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

    void build_per_core_ratio_section(int spacing) {
        per_core_group_ = new QGroupBox();
        per_core_group_->setFlat(true);
        auto *outer_layout = new QVBoxLayout();
        outer_layout->setSpacing(spacing);

        auto *header = new QHBoxLayout();
        header->setSpacing(spacing);
        QLabel *header_label = new QLabel("Set a target ratio for each logical CPU. Current ratios update on Refresh.");
        header_label->setWordWrap(true);
        header->addWidget(header_label, 1);

        per_core_apply_all_btn_ = new QPushButton("Apply all");
        per_core_reset_btn_ = new QPushButton("Reset to P/E");
        header->addWidget(per_core_apply_all_btn_);
        header->addWidget(per_core_reset_btn_);
        outer_layout->addLayout(header);

        per_core_grid_ = new QGridLayout();
        per_core_grid_->setSpacing(spacing);
        per_core_grid_->setVerticalSpacing(spacing);
        per_core_grid_->setHorizontalSpacing(spacing * 2);

        per_core_grid_->addWidget(new QLabel("CPU"), 0, 0, Qt::AlignCenter);
        per_core_grid_->addWidget(new QLabel("Type"), 0, 1, Qt::AlignCenter);
        per_core_grid_->addWidget(new QLabel("Current"), 0, 2, Qt::AlignCenter);
        per_core_grid_->addWidget(new QLabel("Target"), 0, 3, Qt::AlignCenter);
        per_core_grid_->addWidget(new QLabel(""), 0, 4);

        auto *grid_container = new QWidget();
        grid_container->setLayout(per_core_grid_);
        outer_layout->addWidget(grid_container, 1);

        per_core_group_->setLayout(outer_layout);
        per_core_section_ = new CollapsibleSection("Per-core ratios", per_core_group_, spacing);

        connect(per_core_apply_all_btn_, &QPushButton::clicked, this, &MainWindow::apply_all_per_core_ratios);
        connect(per_core_reset_btn_, &QPushButton::clicked, this, &MainWindow::reset_per_core_ratios);
    }

    void populate_per_core_rows(const QList<int> &cpus, const QString &type) {
        for (int cpu : cpus) {
            if (cpu < 0) {
                continue;
            }
            int row = static_cast<int>(per_core_rows_.size()) + 1;
            PerCoreRow r;
            r.cpu = cpu;

            QLabel *cpu_label = new QLabel(QString::number(cpu));
            cpu_label->setAlignment(Qt::AlignCenter);
            r.type_label = new QLabel(type);
            r.type_label->setAlignment(Qt::AlignCenter);
            r.cur_label = new QLabel("-");
            r.cur_label->setAlignment(Qt::AlignCenter);

            r.target_spin = new QSpinBox();
            r.target_spin->setRange(1, 255);
            r.target_spin->setSingleStep(1);
            r.target_spin->setAlignment(Qt::AlignCenter);
            r.target_spin->setMinimumWidth(70);

            r.set_btn = new QPushButton("Set");
            r.set_btn->setProperty("cpu", cpu);
            connect(r.set_btn, &QPushButton::clicked, this, [this, cpu]() {
                apply_per_core_ratio(cpu);
            });

            per_core_grid_->addWidget(cpu_label, row, 0);
            per_core_grid_->addWidget(r.type_label, row, 1);
            per_core_grid_->addWidget(r.cur_label, row, 2);
            per_core_grid_->addWidget(r.target_spin, row, 3);
            per_core_grid_->addWidget(r.set_btn, row, 4);

            per_core_rows_.append(r);
        }
    }

    void build_sensors_tab() {
        sensors_tab_ = new QWidget();
        auto *layout = new QVBoxLayout(sensors_tab_);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(12);

        auto *info = new QLabel("Sensors update only while this tab is visible.");
        QFont info_font = info->font();
        info_font.setItalic(true);
        info->setFont(info_font);
        layout->addWidget(info);

        sensors_table_ = new QTableWidget();
        sensors_table_->setColumnCount(7);
        sensors_table_->setHorizontalHeaderLabels({"CPU", "Type", "Target", "Ratio", "Clock MHz", "Temp °C", "Throttle"});
        sensors_table_->horizontalHeader()->setStretchLastSection(true);
        sensors_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        sensors_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        sensors_table_->setAlternatingRowColors(true);
        sensors_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(sensors_table_, 1);

        auto *footer = new QHBoxLayout();
        footer->addStretch();
        sensors_status_label_ = new QLabel("Waiting...");
        footer->addWidget(sensors_status_label_);
        layout->addLayout(footer);

        sensor_timer_ = new QTimer(this);
        sensor_timer_->setInterval(1000);
        connect(sensor_timer_, &QTimer::timeout, this, &MainWindow::update_sensors);
    }

    void update_sensor_row(SensorRow &row, const CoreSensor *sensor) {
        double mhz = read_current_mhz_for_cpu(row.cpu);
        if (mhz > 0.0) {
            row.freq_item->setText(QString::number(std::llround(mhz)));
        } else {
            row.freq_item->setText("-");
        }

        int temp_md = -1;
        int phys_core = physical_core_for_cpu(row.cpu);
        if (phys_core >= 0) {
            for (const HwmonTemp &t : coretemp_inputs_) {
                if (t.label.compare(QString("Core %1").arg(phys_core), Qt::CaseInsensitive) == 0) {
                    temp_md = read_temp_millidegrees(t.input_path);
                    break;
                }
            }
        }
        if (temp_md < 0) {
            // fallback: match by logical cpu label
            for (const HwmonTemp &t : coretemp_inputs_) {
                if (t.label.compare(QString("Core %1").arg(row.cpu), Qt::CaseInsensitive) == 0 ||
                    t.label.compare(QString("CPU %1").arg(row.cpu), Qt::CaseInsensitive) == 0) {
                    temp_md = read_temp_millidegrees(t.input_path);
                    break;
                }
            }
        }
        if (temp_md > 0) {
            row.temp_item->setText(QString::number(temp_md / 1000));
        } else {
            row.temp_item->setText("-");
        }

        if (sensor) {
            row.type_item->setText(QString(sensor->type));
            row.ratio_item->setText(sensor->ratio_valid ? QString("x%1").arg(sensor->ratio) : "-");
            if (sensor->thermal_valid) {
                row.throttle_item->setText(thermal_status_summary(sensor->thermal));
            } else {
                row.throttle_item->setText("-");
            }
        } else {
            row.ratio_item->setText("-");
            row.throttle_item->setText("-");
        }

        // target ratio from per-core spin boxes if present
        for (const PerCoreRow &pc : per_core_rows_) {
            if (pc.cpu == row.cpu && pc.target_spin) {
                row.target_item->setText(QString("x%1").arg(pc.target_spin->value()));
                break;
            }
        }
    }

    void refresh_sensor_table_structure() {
        coretemp_inputs_ = discover_coretemp_inputs();
        sensor_rows_.clear();
        sensors_table_->setRowCount(0);

        CpuInfo info = read_cpu_info();
        int logical = info.logical_cpus > 0 ? info.logical_cpus : QThread::idealThreadCount();
        if (logical <= 0) {
            logical = 1;
        }

        QList<int> cpus;
        for (int i = 0; i < logical; ++i) {
            if (QFile::exists(QString("/sys/devices/system/cpu/cpu%1").arg(i))) {
                cpus.append(i);
            }
        }
        if (cpus.isEmpty()) {
            for (int i = 0; i < logical; ++i) {
                cpus.append(i);
            }
        }

        sensors_table_->setRowCount(cpus.size());
        for (int i = 0; i < cpus.size(); ++i) {
            int cpu = cpus[i];
            SensorRow row;
            row.cpu = cpu;
            row.type_item = new QTableWidgetItem("?");
            row.target_item = new QTableWidgetItem("-");
            row.ratio_item = new QTableWidgetItem("-");
            row.freq_item = new QTableWidgetItem("-");
            row.temp_item = new QTableWidgetItem("-");
            row.throttle_item = new QTableWidgetItem("-");

            sensors_table_->setItem(i, 0, new QTableWidgetItem(QString::number(cpu)));
            sensors_table_->setItem(i, 1, row.type_item);
            sensors_table_->setItem(i, 2, row.target_item);
            sensors_table_->setItem(i, 3, row.ratio_item);
            sensors_table_->setItem(i, 4, row.freq_item);
            sensors_table_->setItem(i, 5, row.temp_item);
            sensors_table_->setItem(i, 6, row.throttle_item);

            for (int col = 0; col < sensors_table_->columnCount(); ++col) {
                QTableWidgetItem *it = sensors_table_->item(i, col);
                if (it) {
                    it->setTextAlignment(Qt::AlignCenter);
                }
            }

            sensor_rows_.append(row);
        }
    }

    void maybe_start_sensor_timer() {
        if (!sensor_timer_) {
            return;
        }
        bool should_run = isVisible() && !isMinimized() && tab_widget_ && tab_widget_->currentIndex() == 1;
        if (should_run && !sensor_timer_->isActive()) {
            sensor_timer_->start();
            update_sensors();
        } else if (!should_run && sensor_timer_->isActive()) {
            sensor_timer_->stop();
        }
    }

    void apply_per_core_ratio(int cpu) {
        int ratio = 0;
        for (const PerCoreRow &row : per_core_rows_) {
            if (row.cpu == cpu && row.target_spin) {
                ratio = row.target_spin->value();
                break;
            }
        }
        if (ratio <= 0) {
            return;
        }
        QString err;
        if (!backend_.set_cpu_ratio(cpu, ratio, &err)) {
            show_error(QString("Set CPU %1 ratio failed").arg(cpu), err);
            return;
        }
        log_message(QString("Set CPU %1 ratio x%2").arg(cpu).arg(ratio));
    }

    void apply_all_per_core_ratios() {
        for (const PerCoreRow &row : per_core_rows_) {
            if (row.cpu >= 0 && row.target_spin) {
                int ratio = row.target_spin->value();
                QString err;
                if (!backend_.set_cpu_ratio(row.cpu, ratio, &err)) {
                    show_error(QString("Set CPU %1 ratio failed").arg(row.cpu), err);
                    return;
                }
            }
        }
        log_message("Applied per-core ratios.");
    }

    void reset_per_core_ratios() {
        int p_default = p_ratio_spin_->value();
        int e_default = e_ratio_spin_->value();
        for (PerCoreRow &row : per_core_rows_) {
            if (!row.target_spin) {
                continue;
            }
            if (row.type_label) {
                QString t = row.type_label->text();
                if (t == "E" && e_default > 0) {
                    row.target_spin->setValue(e_default);
                } else if (p_default > 0) {
                    row.target_spin->setValue(p_default);
                }
            }
        }
    }

    void update_sensors() {
        if (!backend_ready_) {
            sensors_status_label_->setText("Backend not ready");
            return;
        }
        if (sensor_rows_.isEmpty()) {
            refresh_sensor_table_structure();
        }

        QString err;
        QList<CoreSensor> sensors;
        if (!backend_.read_core_sensors(sensors, &err)) {
            sensors_status_label_->setText("Read failed: " + err);
            return;
        }

        QHash<int, const CoreSensor *> by_cpu;
        for (const CoreSensor &s : sensors) {
            by_cpu.insert(s.cpu, &s);
        }

        for (SensorRow &row : sensor_rows_) {
            update_sensor_row(row, by_cpu.value(row.cpu, nullptr));
        }

        for (PerCoreRow &row : per_core_rows_) {
            const CoreSensor *s = by_cpu.value(row.cpu, nullptr);
            if (s && s->ratio_valid && row.cur_label) {
                row.cur_label->setText(QString("x%1").arg(s->ratio));
            }
        }

        sensors_status_label_->setText(QString("Updated %1 cores").arg(sensor_rows_.size()));
    }

    HelperBackend backend_;
    int power_unit_ = 0;
    double unit_watts_ = 0.0;
    bool did_init_limits_ = false;
    bool did_init_core_uv_ = false;
    bool quitting_ = false;

    QSystemTrayIcon *tray_icon_ = nullptr;
    QMenu *tray_menu_ = nullptr;
    QAction *tray_show_action_ = nullptr;
    QAction *tray_quit_action_ = nullptr;

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
    QCheckBox *close_to_tray_ = nullptr;
    QCheckBox *autostart_enabled_ = nullptr;

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
    QTabWidget *tab_widget_ = nullptr;

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

    CollapsibleSection *per_core_section_ = nullptr;
    QGroupBox *per_core_group_ = nullptr;
    QGridLayout *per_core_grid_ = nullptr;
    QPushButton *per_core_apply_all_btn_ = nullptr;
    QPushButton *per_core_reset_btn_ = nullptr;
    QList<PerCoreRow> per_core_rows_;
    bool per_core_rows_populated_ = false;

    QWidget *sensors_tab_ = nullptr;
    QTableWidget *sensors_table_ = nullptr;
    QLabel *sensors_status_label_ = nullptr;
    QTimer *sensor_timer_ = nullptr;
    QList<SensorRow> sensor_rows_;
    QList<HwmonTemp> coretemp_inputs_;

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
    QApplication::setDesktopFileName("limits_droper");
    app.setQuitOnLastWindowClosed(false);
    MainWindow window;
    window.show();
    return app.exec();
}
