#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QWidget>
#include <QThread>
#include <QIcon>

#include <atomic>
#include <vector>
#include <climits>
#include <ctime>
#include <unordered_set>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

// -------------------- Event model --------------------
struct Event {
    enum Type { MouseMove, MouseButton, Key } type;
    std::int64_t ms_since_start{0};
    int x{0}, y{0};
    int button{0};
    bool pressed{false};
    unsigned int keycode{0};

    QString monitor;      // monitor name
    int relx{0}, rely{0}; // coords relative to that monitor
};

struct MonitorInfo {
    QString name;
    int x, y, width, height;
};

// -------------------- Monitor helpers --------------------
static MonitorInfo findMonitorForPoint(Display* dpy, int x, int y) {
    MonitorInfo result{"",0,0,0,0};
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return result;

    for (int i=0; i<res->noutput; i++) {
        XRROutputInfo* output = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!output) continue;
        if (output->connection == RR_Connected && output->crtc) {
            XRRCrtcInfo* crtc = XRRGetCrtcInfo(dpy, res, output->crtc);
            if (crtc) {
                // Cast width/height to int to avoid signed/unsigned warnings
                int w = static_cast<int>(crtc->width);
                int h = static_cast<int>(crtc->height);
                if (x >= crtc->x && x < crtc->x + w &&
                    y >= crtc->y && y < crtc->y + h) {
                    result.name = output->name;
                    result.x = crtc->x;
                    result.y = crtc->y;
                    result.width = w;
                    result.height = h;
                    XRRFreeCrtcInfo(crtc);
                    XRRFreeOutputInfo(output);
                    break;
                }
                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(res);
    return result;
}

static MonitorInfo findMonitorByName(Display* dpy, const QString& name) {
    MonitorInfo result{"",0,0,0,0};
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return result;

    for (int i=0; i<res->noutput; i++) {
        XRROutputInfo* output = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!output) continue;
        if (output->connection == RR_Connected && output->crtc) {
            XRRCrtcInfo* crtc = XRRGetCrtcInfo(dpy, res, output->crtc);
            if (crtc) {
                if (name == output->name) {
                    result.name = output->name;
                    result.x = crtc->x;
                    result.y = crtc->y;
                    result.width = static_cast<int>(crtc->width);
                    result.height = static_cast<int>(crtc->height);
                    XRRFreeCrtcInfo(crtc);
                    XRRFreeOutputInfo(output);
                    break;
                }
                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(res);
    return result;
}

// -------------------- Time helper --------------------
static std::int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// -------------------- Recorder --------------------
class RecorderThread : public QThread {
    Q_OBJECT
public:
    explicit RecorderThread(QObject *parent = nullptr) : QThread(parent) {}
    std::vector<Event> events;
    void stop() { running = false; }

signals:
    void status(const QString &s);
    void finishedRecording(const QString &summary);

protected:
    void run() override {
        running = true;
        Display *dpy = XOpenDisplay(nullptr);
        if (!dpy) { emit status("Failed to open X display"); return; }

        int xi_opcode, event, error;
        if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) {
            emit status("XInput2 not available");
            XCloseDisplay(dpy);
            return;
        }
        int major = 2, minor = 0;
        if (XIQueryVersion(dpy, &major, &minor) != Success) {
            emit status("XInput2 < 2.0");
            XCloseDisplay(dpy);
            return;
        }

        Window root = DefaultRootWindow(dpy);
        XIEventMask mask{};
        unsigned char m[32] = {0};
        mask.deviceid = XIAllMasterDevices;
        mask.mask_len = sizeof(m);
        mask.mask = m;
        XISetMask(m, XI_RawMotion);
        XISetMask(m, XI_RawButtonPress);
        XISetMask(m, XI_RawButtonRelease);
        XISetMask(m, XI_RawKeyPress);
        XISetMask(m, XI_RawKeyRelease);
        XISelectEvents(dpy, root, &mask, 1);
        XFlush(dpy);

        events.clear();
        auto start = now_ms();
        emit status("Recording...");

        int last_x = -1, last_y = -1;
        std::unordered_set<int> downButtons;

        while (running) {
            if (XPending(dpy) == 0) {
                timespec ts{0, 10 * 1000 * 1000};
                nanosleep(&ts, nullptr);
                continue;
            }
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.xcookie.type != GenericEvent || ev.xcookie.extension != xi_opcode) continue;
            if (!XGetEventData(dpy, &ev.xcookie)) continue;

            auto t = now_ms() - start;
            switch (ev.xcookie.evtype) {
                case XI_RawMotion: {
                    Window r, c; int rx, ry, x, y; unsigned int msk;
                    XQueryPointer(dpy, root, &r, &c, &rx, &ry, &x, &y, &msk);
                    if (x != last_x || y != last_y) {
                        MonitorInfo monitorInfo = findMonitorForPoint(dpy, x, y);
                        Event e;
                        e.type = Event::MouseMove;
                        e.ms_since_start = t;
                        e.x = x;
                        e.y = y;
                        e.monitor = monitorInfo.name;
                        e.relx = x - monitorInfo.x;
                        e.rely = y - monitorInfo.y;
                        events.push_back(e);
                        last_x = x; last_y = y;
                    }
                    break;
                }
                case XI_RawButtonPress:
                case XI_RawButtonRelease: {
                    auto *re = (XIRawEvent*)ev.xcookie.data;
                    Window r, c; int rx, ry, x, y; unsigned int msk;
                    XQueryPointer(dpy, root, &r, &c, &rx, &ry, &x, &y, &msk);
                    bool isPress = (ev.xcookie.evtype == XI_RawButtonPress);
                    if (isPress) downButtons.insert(re->detail);
                    else downButtons.erase(re->detail);

                    MonitorInfo monitorInfo = findMonitorForPoint(dpy, x, y);
                    Event e;
                    e.type = Event::MouseButton;
                    e.ms_since_start = t;
                    e.x = x;
                    e.y = y;
                    e.button = (int)re->detail;
                    e.pressed = isPress;
                    e.monitor = monitorInfo.name;
                    e.relx = x - monitorInfo.x;
                    e.rely = y - monitorInfo.y;
                    events.push_back(e);
                    break;
                }
                case XI_RawKeyPress:
                case XI_RawKeyRelease: {
                    auto *re = (XIRawEvent*)ev.xcookie.data;
                    events.push_back({
                        Event::Key,
                        t,
                        0, 0,                                // x, y
                        0,                                   // button
                        ev.xcookie.evtype == XI_RawKeyPress, // pressed
                        (unsigned)re->detail,                // keycode
                        QString(),                           // monitor
                        0, 0                                 // relx, rely
                    });
                    break;
                }
            }
            XFreeEventData(dpy, &ev.xcookie);
        }

        // Synthesize releases if recording ended while buttons are down
        if (!downButtons.empty()) {
            Window r, c; int rx, ry, x, y; unsigned int msk;
            XQueryPointer(dpy, root, &r, &c, &rx, &ry, &x, &y, &msk);
            auto t = now_ms() - start;
            MonitorInfo monitorInfo = findMonitorForPoint(dpy, x, y);
            for (int b : downButtons) {
                Event e;
                e.type = Event::MouseButton;
                e.ms_since_start = t;
                e.x = x;
                e.y = y;
                e.button = b;
                e.pressed = false;
                e.monitor = monitorInfo.name;
                e.relx = x - monitorInfo.x;
                e.rely = y - monitorInfo.y;
                events.push_back(e);
            }
        }

        XCloseDisplay(dpy);
        emit status("Stopped.");
        emit finishedRecording(QString("Recorded %1 events").arg(events.size()));
    }

private:
    std::atomic<bool> running{false};
};

// -------------------- Player --------------------
class PlayerThread : public QThread {
    Q_OBJECT
public:
    explicit PlayerThread(QObject *parent = nullptr) : QThread(parent) {}
    std::vector<Event> events;
    double speed = 1.0;
    int loops = 1;
    void stop() { running = false; }

signals:
    void status(const QString &s);

protected:
    void run() override {
        if (events.empty()) { emit status("No events to play"); return; }
        running = true;
        Display *dpy = XOpenDisplay(nullptr);
        if (!dpy) { emit status("Failed to open X display"); return; }

        emit status(QString("Playing (%1 loops, speed x%2)...").arg(loops).arg(speed));

        auto auto_release = [&](int button) {
            timespec ts{0, 30 * 1000 * 1000}; // 30ms click
            nanosleep(&ts, nullptr);
            XTestFakeButtonEvent(dpy, button, False, 0);
            XFlush(dpy);
        };

        for (int k = 0; k < loops && running; ++k) {
            auto start = now_ms();
            for (size_t i = 0; i < events.size() && running; ++i) {
                const auto &e = events[i];

                auto target = start + (std::int64_t)(e.ms_since_start / speed);
                auto n = now_ms();
                if (target > n) {
                    auto delta = target - n;
                    timespec ts{(time_t)(delta/1000), (long)((delta%1000)*1000000)};
                    nanosleep(&ts, nullptr);
                }

                switch (e.type) {
                    case Event::MouseMove: {
                        int absx = e.x;
                        int absy = e.y;
                        if (!e.monitor.isEmpty()) {
                            MonitorInfo monitorInfo = findMonitorByName(dpy, e.monitor);
                            if (!monitorInfo.name.isEmpty()) {
                                absx = monitorInfo.x + e.relx;
                                absy = monitorInfo.y + e.rely;
                            }
                        }
                        XTestFakeMotionEvent(dpy, -1, absx, absy, 0);
                        XFlush(dpy);
                        break;
                    }

                    case Event::MouseButton: {
                        int absx = e.x;
                        int absy = e.y;
                        if (!e.monitor.isEmpty()) {
                            MonitorInfo monitorInfo = findMonitorByName(dpy, e.monitor);
                            if (!monitorInfo.name.isEmpty()) {
                                absx = monitorInfo.x + e.relx;
                                absy = monitorInfo.y + e.rely;
                                XTestFakeMotionEvent(dpy, -1, absx, absy, 0); // move before click
                            }
                        }
                        XTestFakeButtonEvent(dpy, e.button, e.pressed, 0);
                        XFlush(dpy);

                        if (e.pressed) {
                            bool nextIsRelease = false;
                            if (i + 1 < events.size()) {
                                const auto &next = events[i+1];
                                nextIsRelease = (next.type == Event::MouseButton &&
                                                 next.button == e.button &&
                                                 !next.pressed);
                            }
                            if (!nextIsRelease) {
                                auto_release(e.button);
                            } else {
                                timespec ts{0, 15 * 1000 * 1000}; // 15ms visible tap
                                nanosleep(&ts, nullptr);
                            }
                        }
                        break;
                    }

                    case Event::Key:
                        XTestFakeKeyEvent(dpy, e.keycode, e.pressed, 0);
                        XFlush(dpy);
                        break;
                }
            }
        }

        // Safety: release any buttons that might still be down
        for (int b = 1; b <= 7; ++b) {
            XTestFakeButtonEvent(dpy, b, False, 0);
        }
        XFlush(dpy);

        XCloseDisplay(dpy);
        emit status("Playback finished.");
    }

private:
    std::atomic<bool> running{false};
};

// -------------------- Ctrl-stop watcher --------------------
class StopWatcher : public QThread {
    Q_OBJECT
public:
    explicit StopWatcher(PlayerThread *p, QObject *parent = nullptr)
        : QThread(parent), player(p) {}
    void stop() { running = false; }

protected:
    void run() override {
        running = true;
        Display *dpy = XOpenDisplay(nullptr);
        if (!dpy) return;

        int xi_opcode, event, error;
        if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error)) {
            XCloseDisplay(dpy);
            return;
        }

        Window root = DefaultRootWindow(dpy);

        XIEventMask mask{};
        unsigned char m[32] = {0};
        mask.deviceid = XIAllMasterDevices;
        mask.mask_len = sizeof(m);
        mask.mask = m;
        XISetMask(m, XI_RawKeyPress);
        XISelectEvents(dpy, root, &mask, 1);
        XFlush(dpy);

        while (running) {
            if (XPending(dpy) == 0) {
                usleep(10000);
                continue;
            }
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.xcookie.type == GenericEvent && ev.xcookie.extension == xi_opcode) {
                if (XGetEventData(dpy, &ev.xcookie)) {
                    if (ev.xcookie.evtype == XI_RawKeyPress) {
                        auto *re = (XIRawEvent*)ev.xcookie.data;
                        if (re->detail == 37 || re->detail == 105) { // Left/Right Ctrl
                            player->stop();
                            running = false;
                        }
                    }
                    XFreeEventData(dpy, &ev.xcookie);
                }
            }
        }

        XCloseDisplay(dpy);
    }

private:
    PlayerThread *player;
    std::atomic<bool> running{false};
};

// -------------------- Main window --------------------
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        recorder = new RecorderThread(this);
        player = new PlayerThread(this);
        stopWatcher = new StopWatcher(player, this);
        setupUi();

        connect(recorder, &RecorderThread::status, status, &QLabel::setText);
        connect(recorder, &RecorderThread::finishedRecording, this, [this](const QString &s){
            status->setText(s);
            recorded = recorder->events;     // overwrite previous recording
            btnRecord->setText("Record");
            btnPlay->setEnabled(true);
            btnSave->setEnabled(!recorded.empty());
        });
        connect(player, &PlayerThread::status, status, &QLabel::setText);
    }

    ~MainWindow() override {
        if (recorder->isRunning()) recorder->stop();
        if (player->isRunning()) player->stop();
        stopWatcher->stop();
        stopWatcher->wait();
        recorder->wait();
        player->wait();
    }

private:
    RecorderThread *recorder{nullptr};
    PlayerThread *player{nullptr};
    StopWatcher *stopWatcher{nullptr};
    std::vector<Event> recorded;

    QLabel *status{nullptr};
    QDoubleSpinBox *spinSpeed{nullptr};
    QSpinBox *spinLoops{nullptr};
    QCheckBox *chkInfinite{nullptr};
    QPushButton *btnRecord{nullptr};
    QPushButton *btnPlay{nullptr};
    QPushButton *btnSave{nullptr};
    QPushButton *btnLoad{nullptr};

    void setupUi() {
        auto *central = new QWidget(this);
        auto *v = new QVBoxLayout(central);

        auto *h1 = new QHBoxLayout();
        btnRecord = new QPushButton("Record");
        btnPlay = new QPushButton("Play");
        btnSave = new QPushButton("Save");
        btnLoad = new QPushButton("Load");
        h1->addWidget(btnRecord);
        h1->addWidget(btnPlay);
        h1->addWidget(btnSave);
        h1->addWidget(btnLoad);

        auto *h2 = new QHBoxLayout();
        spinSpeed = new QDoubleSpinBox();
        spinSpeed->setRange(0.1, 5.0);
        spinSpeed->setValue(1.0);
        spinLoops = new QSpinBox();
        spinLoops->setRange(1, 999);
        spinLoops->setValue(1);
        chkInfinite = new QCheckBox("Infinite loop");
        h2->addWidget(new QLabel("Speed:"));
        h2->addWidget(spinSpeed);
        h2->addWidget(new QLabel("Loops:"));
        h2->addWidget(spinLoops);
        h2->addWidget(chkInfinite);

        status = new QLabel("Ready.");

        v->addLayout(h1);
        v->addLayout(h2);
        v->addWidget(status);
        setCentralWidget(central);

        btnPlay->setEnabled(false);
        btnSave->setEnabled(false);

        // Record overwrite semantics
        connect(btnRecord, &QPushButton::clicked, this, [this]() {
            if (!recorder->isRunning()) {
                recorded.clear();
                btnPlay->setEnabled(false);
                btnSave->setEnabled(false);
                recorder->start();
                btnRecord->setText("Stop");
                btnPlay->setEnabled(false);
            } else {
                recorder->stop();
                btnRecord->setText("Record");
            }
        });

        // Play/Stop + Ctrl-stop watcher
        connect(btnPlay, &QPushButton::clicked, this, [this]() {
            if (!player->isRunning()) {
                if (!recorded.empty()) {
                    player->events = recorded;
                    player->speed = spinSpeed->value();
                    player->loops = chkInfinite->isChecked() ? INT_MAX : spinLoops->value();
                    player->start();
                    if (!stopWatcher->isRunning()) stopWatcher->start();
                    btnPlay->setText("Stop");
                    btnRecord->setEnabled(false);
                }
            } else {
                player->stop();
                stopWatcher->stop();
                stopWatcher->wait();
                btnPlay->setText("Play");
                btnRecord->setEnabled(true);
            }
        });

        // Reset Play when finished
        connect(player, &PlayerThread::status, this, [this](const QString &s) {
            status->setText(s);
            if (s.contains("finished", Qt::CaseInsensitive) ||
                s.contains("Stopped", Qt::CaseInsensitive)) {
                btnPlay->setText("Play");
                stopWatcher->stop();
                stopWatcher->wait();
                btnRecord->setEnabled(true);
            }
        });

        // Save current recording only (.recq)
        connect(btnSave, &QPushButton::clicked, this, [this]() {
            if (recorded.empty()) return;
            QString path = QFileDialog::getSaveFileName(this, "Save macro", {}, "Macro (*.recq)");
            if (path.isEmpty()) return;
            if (!path.endsWith(".recq")) path += ".recq";
            saveRecq(path, recorded);
        });

        // Load replaces current recording (.recq)
        connect(btnLoad, &QPushButton::clicked, this, [this]() {
            QString path = QFileDialog::getOpenFileName(this, "Load macro", {}, "Macro (*.recq)");
            if (path.isEmpty()) return;
            recorded = loadRecq(path);
            btnPlay->setEnabled(!recorded.empty());
            btnSave->setEnabled(!recorded.empty());
            status->setText(QString("Loaded %1 events").arg(recorded.size()));
        });
    }

    // Save/Load helpers for .recq (JSON with format marker)
    static bool saveRecq(const QString &path, const std::vector<Event> &evs) {
        QJsonArray arr;
        for (const auto &e : evs) {
            QJsonObject o;
            o["t"] = (double)e.ms_since_start;
            if (e.type == Event::MouseMove) {
                o["type"] = "mm"; o["x"] = e.x; o["y"] = e.y;
            } else if (e.type == Event::MouseButton) {
                o["type"] = "mb"; o["x"] = e.x; o["y"] = e.y;
                o["btn"] = e.button; o["down"] = e.pressed;
            } else {
                o["type"] = "key"; o["code"] = (int)e.keycode; o["down"] = e.pressed;
            }
            arr.append(o);
        }
        QJsonObject root;
        root["format"] = "recq-v1";
        root["events"] = arr;

        QJsonDocument doc(root);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(doc.toJson(QJsonDocument::Compact));
        return true;
    }

    static std::vector<Event> loadRecq(const QString &path) {
        std::vector<Event> out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return out;
        auto data = f.readAll();
        auto doc = QJsonDocument::fromJson(data);

        // Accept either object with "events" or legacy pure array
        if (doc.isObject()) {
            auto root = doc.object();
            auto fmt = root.value("format").toString();
            if (!fmt.startsWith("recq-")) {
                // Optional: reject if not recq, but let's be permissive
            }
            auto arr = root.value("events").toArray();
            for (auto v : arr) {
                auto o = v.toObject();
                Event e{};
                e.ms_since_start = (std::int64_t)o.value("t").toDouble();
                auto type = o.value("type").toString();

                if (type == "mm") {
                    e.type = Event::MouseMove;
                    e.x = o.value("x").toInt();
                    e.y = o.value("y").toInt();
                } else if (type == "mb") {
                    e.type = Event::MouseButton;
                    e.x = o.value("x").toInt();
                    e.y = o.value("y").toInt();
                    e.button = o.value("btn").toInt();
                    e.pressed = o.value("down").toBool();
                } else if (type == "key") {
                    e.type = Event::Key;
                    e.keycode = o.value("code").toInt();
                    e.pressed = o.value("down").toBool();
                }
                out.push_back(e);
            }
        } else if (doc.isArray()) {
            // Legacy: pure array (like earlier versions)
            for (auto v : doc.array()) {
                auto o = v.toObject();
                Event e{};
                e.ms_since_start = (std::int64_t)o.value("t").toDouble();
                auto type = o.value("type").toString();

                if (type == "mm") {
                    e.type = Event::MouseMove;
                    e.x = o.value("x").toInt();
                    e.y = o.value("y").toInt();
                } else if (type == "mb") {
                    e.type = Event::MouseButton;
                    e.x = o.value("x").toInt();
                    e.y = o.value("y").toInt();
                    e.button = o.value("btn").toInt();
                    e.pressed = o.value("down").toBool();
                } else if (type == "key") {
                    e.type = Event::Key;
                    e.keycode = o.value("code").toInt();
                    e.pressed = o.value("down").toBool();
                }
                out.push_back(e);
            }
        }

        return out;
    }
};

// -------------------- main --------------------
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    app.setWindowIcon(QIcon(":/icons/BiggerTask.svg"));

    MainWindow w;
    w.setWindowTitle("BiggerTask");
    w.show();
    return app.exec();
}

#include "BiggerTask.moc"
