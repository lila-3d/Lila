#include "ui/RenderOutputDialog.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/RenderSettings.h"
#include "ui/FileDialog.h"
#include "ui/LocateFlash.h"
#include "ui/RenderOutput.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
QPointer<RenderOutputDialog> gRenderOutputDialog;

const nlohmann::json& output()
{
    return scene::renderSettings().output;
}

template <class T> T value(const char* key, T fallback)
{
    const nlohmann::json& o = output();
    auto it = o.find(key);
    if (it == o.end() || it->is_null())
        return fallback;
    try
    {
        return it->get<T>();
    }
    catch (const nlohmann::json::exception&)
    {
        return fallback;
    }
}

QDoubleSpinBox* doubleSpin(QWidget* parent, double lo, double hi, double step, int decimals)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setKeyboardTracking(false);
    return s;
}

/** A line edit with a browse button beside it. */
QWidget* pathRow(QWidget* parent, QLineEdit*& edit, std::function<QString()> browse)
{
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit(row);
    auto* b = new QToolButton(row);
    b->setText("...");
    h->addWidget(edit, 1);
    h->addWidget(b);
    QLineEdit* e = edit;
    QObject::connect(b, &QToolButton::clicked, row, [e, browse] {
        QString path = browse();
        if (!path.isEmpty())
        {
            e->setText(path);
            Q_EMIT e->editingFinished();
        }
    });
    return row;
}
} // namespace

RenderOutputDialog::RenderOutputDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Render Output");
    auto* v = new QVBoxLayout(this);

    auto* look = new QGroupBox("Picture", this);
    auto* lf = new QFormLayout(look);
    lf->setLabelAlignment(Qt::AlignRight);
    mCamera = new QComboBox(look);
    lf->addRow("Camera", mCamera);
    mScale = doubleSpin(look, 5.0, 800.0, 25.0, 0);
    mScale->setSuffix(" %");
    mSize = new QLabel(look);
    auto* scaleRow = new QWidget(look);
    auto* sh = new QHBoxLayout(scaleRow);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(mScale);
    sh->addWidget(mSize, 1);
    lf->addRow("Resolution", scaleRow);
    mSamples = new QSpinBox(look);
    mSamples->setRange(1, 4096);
    mSamples->setKeyboardTracking(false);
    mSamples->setToolTip("Jittered frames averaged into each picture. More smooths edges, depth of field and motion blur.");
    lf->addRow("Samples", mSamples);
    mMotionBlur = new QCheckBox("Motion blur", look);
    mTransparent = new QCheckBox("Transparent background", look);
    lf->addRow("", mMotionBlur);
    lf->addRow("", mTransparent);
    v->addWidget(look);

    auto* out = new QGroupBox("Output", this);
    auto* of = new QFormLayout(out);
    of->setLabelAlignment(Qt::AlignRight);
    auto* range = new QWidget(out);
    auto* rh = new QHBoxLayout(range);
    rh->setContentsMargins(0, 0, 0, 0);
    mStart = doubleSpin(range, -1e6, 1e6, 1.0, 0);
    mEnd = doubleSpin(range, -1e6, 1e6, 1.0, 0);
    mStep = doubleSpin(range, 0.01, 1000.0, 1.0, 2);
    rh->addWidget(mStart);
    rh->addWidget(new QLabel("to", range));
    rh->addWidget(mEnd);
    rh->addWidget(new QLabel("by", range));
    rh->addWidget(mStep);
    of->addRow("Frames", range);
    of->addRow("Images", pathRow(out, mImages, [this] {
                   return fileDialog::getSaveFileName(this, "Rendered Images", mImages->text(),
                                                      "Images (*.png *.exr *.jpg *.tif *.tga *.psd)");
               }));
    mImages->setToolTip("#### stands for the frame number. Leave empty to write video only.");
    of->addRow("Video", pathRow(out, mVideo, [this] { return fileDialog::getSaveFileName(this, "Video", mVideo->text(), "MP4 video (*.mp4)"); }));
    mVideo->setPlaceholderText("none");
    auto* codecRow = new QWidget(out);
    auto* ch = new QHBoxLayout(codecRow);
    ch->setContentsMargins(0, 0, 0, 0);
    mCodec = new QComboBox(codecRow);
    mCodec->addItem("H.264", "h264");
    mCodec->addItem("HEVC", "hevc");
    mQuality = doubleSpin(codecRow, 0.0, 1.0, 0.1, 2);
    ch->addWidget(mCodec);
    ch->addWidget(new QLabel("Quality", codecRow));
    ch->addWidget(mQuality, 1);
    of->addRow("Codec", codecRow);
    v->addWidget(out);

    mPreview = new QLabel(this);
    mPreview->setMinimumSize(320, 180);
    mPreview->setAlignment(Qt::AlignCenter);
    mPreview->setText("Rendered frames show here.");
    v->addWidget(mPreview, 1);

    auto* buttons = new QHBoxLayout;
    auto* frame = new QPushButton("Render Frame", this);
    auto* sequence = new QPushButton("Render Sequence", this);
    auto* folder = new QPushButton("Open Folder", this);
    buttons->addWidget(frame);
    buttons->addWidget(sequence);
    buttons->addStretch(1);
    buttons->addWidget(folder);
    v->addLayout(buttons);

    connect(mCamera, &QComboBox::currentIndexChanged, this, [this](int) { store("camera", mCamera->currentData().toString().toStdString()); });
    connect(mScale, &QDoubleSpinBox::valueChanged, this, [this](double x) {
        store("scale", x);
        refresh();
    });
    connect(mSamples, &QSpinBox::valueChanged, this, [this](int n) { store("samples", n); });
    connect(mMotionBlur, &QCheckBox::toggled, this, [this](bool on) { store("motionBlur", on); });
    connect(mTransparent, &QCheckBox::toggled, this, [this](bool on) { store("transparent", on); });
    connect(mStart, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("start", x); });
    connect(mEnd, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("end", x); });
    connect(mStep, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("step", x); });
    connect(mImages, &QLineEdit::editingFinished, this, [this] { store("path", mImages->text().toStdString()); });
    connect(mVideo, &QLineEdit::editingFinished, this, [this] { store("video", mVideo->text().toStdString()); });
    connect(mCodec, &QComboBox::currentIndexChanged, this, [this](int) { store("codec", mCodec->currentData().toString().toStdString()); });
    connect(mQuality, &QDoubleSpinBox::valueChanged, this, [this](double x) { store("quality", x); });
    connect(frame, &QPushButton::clicked, this, [] { renderFrame(); });
    connect(sequence, &QPushButton::clicked, this, [] { renderSequence(); });
    connect(folder, &QPushButton::clicked, this, [this] {
        std::string pattern = mImages->text().isEmpty() ? mVideo->text().toStdString() : mImages->text().toStdString();
        if (pattern.empty())
            pattern = renderOutput::defaultPattern();
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(pattern)).absolutePath()));
    });

    mListener = scene::addRenderSettingsListener([this] { refresh(); });
    refresh();
}

RenderOutputDialog::~RenderOutputDialog()
{
    scene::removeRenderSettingsListener(mListener);
}

void RenderOutputDialog::store(const char* key, const nlohmann::json& v)
{
    if (output().value(key, nlohmann::json()) == v)
        return;
    scene::renderSettings().output[key] = v;
    scene::renderSettingsChanged();
}

void RenderOutputDialog::refresh()
{
    const scene::RenderSettings& r = scene::renderSettings();
    QSignalBlocker b0(mCamera), b1(mScale), b2(mSamples), b3(mMotionBlur), b4(mTransparent), b5(mStart), b6(mEnd), b7(mStep), b8(mCodec),
        b9(mQuality);
    mCamera->clear();
    mCamera->addItem("Active view", QString());
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->type().isA("camera"))
            mCamera->addItem(QString::fromStdString(n->name()), QString::fromStdString(n->name()));
    const int cam = mCamera->findData(QString::fromStdString(value<std::string>("camera", "")));
    mCamera->setCurrentIndex(std::max(cam, 0));
    const double scale = value("scale", 100.0);
    mScale->setValue(scale);
    mSize->setText(QString("%1 x %2").arg(int(std::lround(r.width * scale / 100.0))).arg(int(std::lround(r.height * scale / 100.0))));
    mSamples->setValue(value("samples", 64));
    mMotionBlur->setChecked(value("motionBlur", false));
    mTransparent->setChecked(value("transparent", false));
    const scene::anim::Playback& pb = scene::anim::playback();
    mStart->setValue(value("start", std::round(scene::anim::frameOf(pb.minTime))));
    mEnd->setValue(value("end", std::round(scene::anim::frameOf(pb.maxTime))));
    mStep->setValue(value("step", 1.0));
    if (!mImages->hasFocus())
    {
        mImages->setText(QString::fromStdString(value<std::string>("path", "")));
        mImages->setPlaceholderText(QString::fromStdString(renderOutput::defaultPattern()));
    }
    if (!mVideo->hasFocus())
        mVideo->setText(QString::fromStdString(value<std::string>("video", "")));
    mCodec->setCurrentIndex(std::max(mCodec->findData(QString::fromStdString(value<std::string>("codec", "h264"))), 0));
    mQuality->setValue(value("quality", 0.6));
}

void RenderOutputDialog::showImage(const QString& path)
{
    QPixmap pix(path);
    if (pix.isNull())
        return;
    mPreview->setPixmap(pix.scaled(mPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

std::string RenderOutputDialog::renderFrame()
{
    try
    {
        std::string path = ops::execute("render", nlohmann::json::object(), CallSource::UI).get<std::string>();
        log::info("Rendered {}", path);
        if (gRenderOutputDialog)
            gRenderOutputDialog->showImage(QString::fromStdString(path));
        return path;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return {};
}

void RenderOutputDialog::renderSequence()
{
    try
    {
        nlohmann::json r = ops::execute("renderSequence", nlohmann::json::object(), CallSource::UI);
        if (r.contains("path") && r["path"].is_array() && !r["path"].empty())
        {
            const std::string last = r["path"].back().get<std::string>();
            log::info("Rendered {} frames to {}", r.value("frames", 0), last);
            if (gRenderOutputDialog)
                gRenderOutputDialog->showImage(QString::fromStdString(last));
        }
        if (const std::string video = r.value("video", std::string()); !video.empty())
            log::info("Rendered video {}", video);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void RenderOutputDialog::open(QWidget* parent)
{
    if (!gRenderOutputDialog)
    {
        gRenderOutputDialog = new RenderOutputDialog(parent);
        gRenderOutputDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    gRenderOutputDialog->show();
    gRenderOutputDialog->raise();
    gRenderOutputDialog->activateWindow();
    flashLocate(gRenderOutputDialog);
}
} // namespace lila::ui
