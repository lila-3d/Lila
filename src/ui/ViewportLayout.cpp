#include "ui/ViewportLayout.h"

#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"
#include "ui/ViewportToolbar.h"
#include "ui/ViewportWidget.h"

#include <QCursor>
#include <QEvent>
#include <QGridLayout>
#include <QPointer>

namespace lila::ui
{
ViewportLayout::ViewportLayout(QWidget* parent) : QWidget(parent)
{
    mGrid = new QGridLayout(this);
    mGrid->setContentsMargins(0, 0, 0, 0);
    mGrid->setSpacing(2);
    const char* views[4] = {"top", "persp", "front", "right"};
    for (int i = 0; i < 4; ++i)
    {
        mPanels[i] = new ViewportPanel(this);
        mViews[i] = mPanels[i]->view();
        mViews[i]->setView(views[i]);
        mViews[i]->installEventFilter(this);
        mGrid->addWidget(mPanels[i], i / 2, i % 2);
    }
    mActive = mViews[1];
    mQuad = PrefStore::get().value<bool>("general", "/viewport/quad", false);
    relayout();

    QPointer<ViewportLayout> self(this);
    io::registerUiSection(
        "viewports",
        [self]() -> nlohmann::json {
            if (!self)
                return nullptr;
            nlohmann::json names = nlohmann::json::array(), cameras = nlohmann::json::array();
            bool any = false;
            for (ViewportWidget* v : self->mViews)
            {
                const scene::Node* n = scene::Graph::current().node(v->lookThroughCamera());
                names.push_back(n ? n->name() : std::string());
                any |= n != nullptr;
                cameras.push_back(v->cameraState());
            }
            nlohmann::json section{{"cameras", cameras}};
            if (any)
                section["lookThrough"] = names;
            for (size_t i = 0; i < self->mViews.size(); ++i)
                if (self->mViews[i] == self->mActive)
                    section["active"] = i;
            return section;
        },
        [self](const nlohmann::json& section) {
            if (!self)
                return;
            nlohmann::json names = section.is_object() ? section.value("lookThrough", nlohmann::json::array()) : nlohmann::json::array();
            nlohmann::json cameras = section.is_object() ? section.value("cameras", nlohmann::json::array()) : nlohmann::json::array();
            for (size_t i = 0; i < self->mViews.size() && i < cameras.size(); ++i)
                self->mViews[i]->restoreCameraState(cameras[i]);
            self->mCamerasRestored = !cameras.empty();
            for (size_t i = 0; i < self->mViews.size(); ++i)
            {
                ViewportWidget* v = self->mViews[i];
                std::string name = i < names.size() && names[i].is_string() ? names[i].get<std::string>() : std::string();
                const scene::Node* n = name.empty() ? nullptr : scene::Graph::current().find(name);
                if (n && n->type().isA("camera"))
                    v->lookThrough(n->id());
                else if (v->lookThroughCamera() != scene::kNoNode)
                    v->stopLookingThrough();
            }
            if (size_t active = section.is_object() ? section.value("active", self->mViews.size()) : self->mViews.size(); active < self->mViews.size())
            {
                self->mActive = self->mViews[active];
                self->relayout();
            }
        });
}

void ViewportLayout::frameUnlessRestored()
{
    if (!mCamerasRestored && mActive)
        mActive->frameSelection(true);
    mCamerasRestored = false;
}

void ViewportLayout::relayout()
{
    for (int i = 0; i < 4; ++i)
        mPanels[i]->setVisible(mQuad || mViews[i] == mActive);
    if (mActive)
        mActive->setFocus();
}

void ViewportLayout::setQuad(bool on)
{
    if (on == mQuad)
        return;
    mQuad = on;
    PrefStore::get().set("general", "/viewport/quad", on);
    relayout();
}

void ViewportLayout::toggleQuad()
{
    // Like Maya, the view under the cursor is the one that stays (or the one that fills the area).
    if (mQuad)
        for (ViewportWidget* v : mViews)
            if (v->rect().contains(v->mapFromGlobal(QCursor::pos())))
                mActive = v;
    setQuad(!mQuad);
}

bool ViewportLayout::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::FocusIn)
        if (auto* v = qobject_cast<ViewportWidget*>(obj))
            mActive = v;
    return QWidget::eventFilter(obj, e);
}
} // namespace lila::ui
