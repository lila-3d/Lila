#include "ui/RenderSettingsDialog.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "scene/RenderSettings.h"
#include "ui/SpinScrub.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
void runUi(const char* command, const json& call)
{
    try
    {
        cmds::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

const char* kCameras[] = {"persp", "top", "front", "right", "left", "back", "bottom"};
} // namespace

RenderSettingsDialog::RenderSettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Render Settings");
    auto* v = new QVBoxLayout(this);

    auto* res = new QGroupBox("Resolution", this);
    auto* rf = new QFormLayout(res);
    rf->setLabelAlignment(Qt::AlignRight);
    mPreset = new QComboBox(res);
    mPreset->addItem("Custom");
    for (const scene::ResolutionPreset& p : scene::resolutionPresets())
        mPreset->addItem(QString("%1  (%2 x %3)").arg(p.name).arg(p.width).arg(p.height), QString(p.name));
    rf->addRow("Preset", mPreset);
    mWidth = new QSpinBox(res);
    mHeight = new QSpinBox(res);
    for (QSpinBox* s : {mWidth, mHeight})
    {
        s->setRange(1, 65536);
        s->setKeyboardTracking(false);
    }
    const scene::RenderSettings defaults;
    const scene::CameraSettings camDefaults;
    setSpinDefault(mWidth, defaults.width);
    setSpinDefault(mHeight, defaults.height);
    rf->addRow("Width", mWidth);
    rf->addRow("Height", mHeight);
    mDeviceAspect = new QDoubleSpinBox(res);
    mDeviceAspect->setRange(0.01, 100.0);
    mDeviceAspect->setDecimals(4);
    mDeviceAspect->setSingleStep(0.01);
    mDeviceAspect->setKeyboardTracking(false);
    setSpinDefault(mDeviceAspect, defaults.deviceAspect());
    rf->addRow("Device aspect", mDeviceAspect);
    mPixelAspect = new QLabel(res);
    rf->addRow("Pixel aspect", mPixelAspect);
    v->addWidget(res);

    auto* cam = new QGroupBox("Camera", this);
    auto* cf = new QFormLayout(cam);
    cf->setLabelAlignment(Qt::AlignRight);
    mCamera = new QComboBox(cam);
    for (const char* c : kCameras)
        mCamera->addItem(c);
    cf->addRow("Camera", mCamera);
    mFilmFit = new QComboBox(cam);
    for (const char* f : {"Fill", "Horizontal", "Vertical", "Overscan"})
        mFilmFit->addItem(f, QString(f).toLower());
    setComboDefault(mFilmFit, int(camDefaults.filmFit));
    cf->addRow("Film fit", mFilmFit);
    mOverscan = new QDoubleSpinBox(cam);
    mOverscan->setRange(0.1, 10.0);
    mOverscan->setSingleStep(0.05);
    mOverscan->setKeyboardTracking(false);
    setSpinDefault(mOverscan, camDefaults.overscan);
    cf->addRow("Overscan", mOverscan);
    mGate = new QCheckBox(cam);
    setCheckDefault(mGate, camDefaults.resolutionGate);
    cf->addRow("Resolution gate", mGate);
    mFilmGate = new QCheckBox(cam);
    setCheckDefault(mFilmGate, camDefaults.filmGate);
    cf->addRow("Film gate", mFilmGate);
    mMask = new QCheckBox(cam);
    setCheckDefault(mMask, camDefaults.gateMask);
    cf->addRow("Gate mask", mMask);
    mMaskOpacity = new QDoubleSpinBox(cam);
    mMaskOpacity->setRange(0.0, 1.0);
    mMaskOpacity->setSingleStep(0.05);
    mMaskOpacity->setKeyboardTracking(false);
    setSpinDefault(mMaskOpacity, camDefaults.gateMaskOpacity);
    cf->addRow("Mask opacity", mMaskOpacity);
    v->addWidget(cam);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    auto* close = new QPushButton("Close", this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(close);
    v->addLayout(bottom);

    connect(mPreset, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i > 0)
            runUi("setRenderResolution", {{"preset", mPreset->itemData(i).toString().toStdString()}});
    });
    connect(mWidth, &QSpinBox::valueChanged, this, [](int w) { runUi("setRenderResolution", {{"width", w}}); });
    connect(mHeight, &QSpinBox::valueChanged, this, [](int h) { runUi("setRenderResolution", {{"height", h}}); });
    connect(mDeviceAspect, &QDoubleSpinBox::valueChanged, this, [](double a) { runUi("setRenderResolution", {{"deviceAspect", a}}); });
    connect(mCamera, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
    auto camFlag = [this](const char* flag, const json& value) {
        runUi("cameraSettings", {{"args", json::array({camera()})}, {flag, value}});
    };
    connect(mFilmFit, &QComboBox::currentIndexChanged, this,
            [this, camFlag](int) { camFlag("filmFit", mFilmFit->currentData().toString().toStdString()); });
    connect(mOverscan, &QDoubleSpinBox::valueChanged, this, [camFlag](double o) { camFlag("overscan", o); });
    connect(mGate, &QCheckBox::toggled, this, [camFlag](bool on) { camFlag("gate", on); });
    connect(mFilmGate, &QCheckBox::toggled, this, [camFlag](bool on) { camFlag("filmGate", on); });
    connect(mMask, &QCheckBox::toggled, this, [camFlag](bool on) { camFlag("gateMask", on); });
    connect(mMaskOpacity, &QDoubleSpinBox::valueChanged, this, [camFlag](double o) { camFlag("maskOpacity", o); });

    mListener = scene::addRenderSettingsListener([this] { refresh(); });
    refresh();
}

RenderSettingsDialog::~RenderSettingsDialog()
{
    scene::removeRenderSettingsListener(mListener);
}

std::string RenderSettingsDialog::camera() const
{
    return mCamera->currentText().toStdString();
}

void RenderSettingsDialog::refresh()
{
    const scene::RenderSettings& r = scene::renderSettings();
    QSignalBlocker b0(mPreset), b1(mWidth), b2(mHeight), b3(mDeviceAspect), b4(mFilmFit), b5(mOverscan), b6(mGate),
        b7(mFilmGate), b8(mMask), b9(mMaskOpacity);
    int preset = 0;
    const auto& ps = scene::resolutionPresets();
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].width == r.width && ps[i].height == r.height && r.pixelAspect == 1.0)
            preset = int(i) + 1;
    mPreset->setCurrentIndex(preset);
    mWidth->setValue(r.width);
    mHeight->setValue(r.height);
    mDeviceAspect->setValue(r.deviceAspect());
    mPixelAspect->setText(QString::number(r.pixelAspect, 'f', 4));

    scene::CameraSettings c = r.camera(camera());
    mFilmFit->setCurrentIndex(int(c.filmFit));
    mOverscan->setValue(c.overscan);
    mGate->setChecked(c.resolutionGate);
    mFilmGate->setChecked(c.filmGate);
    mMask->setChecked(c.gateMask);
    mMaskOpacity->setValue(c.gateMaskOpacity);
    mMaskOpacity->setEnabled(c.gateMask);
}

void RenderSettingsDialog::open(QWidget* parent, const std::string& camera)
{
    static QPointer<RenderSettingsDialog> dialog;
    if (!dialog)
    {
        dialog = new RenderSettingsDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    if (!camera.empty())
    {
        int i = dialog->mCamera->findText(QString::fromStdString(camera));
        if (i < 0)
        {
            dialog->mCamera->addItem(QString::fromStdString(camera));
            i = dialog->mCamera->count() - 1;
        }
        dialog->mCamera->setCurrentIndex(i);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
} // namespace lila::ui
