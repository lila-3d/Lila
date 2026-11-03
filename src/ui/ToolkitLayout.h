#pragma once

#include "core/PrefStore.h"
#include "ui/FixedRowLayout.h"

#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

namespace lila::ui
{
/** Buttons per row in the toolkit panels' button grids. */
inline constexpr int kToolkitColumns = 4;

/** A widget holding a FixedRowLayout of buttons, returned in `layout`. */
inline QWidget* rowBox(QWidget* parent, FixedRowLayout** layout, int columns = 0)
{
    auto* w = new QWidget(parent);
    *layout = new FixedRowLayout(columns, 3);
    (*layout)->setContentsMargins(2, 0, 2, 2);
    w->setLayout(*layout);
    return w;
}

/**
 * `content` under a header button that collapses it. Whether it is collapsed is remembered in the general prefs under
 * `collapsedPrefs` + "/" + title. `headerOut` gets the header, for callers that open or close it themselves.
 */
inline QWidget* collapsibleSection(const QString& title, QWidget* content, QWidget* parent, const char* collapsedPrefs,
                                   bool openByDefault = true, QToolButton** headerOut = nullptr)
{
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* header = new QToolButton(box);
    header->setText(title);
    header->setCheckable(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setStyleSheet("QToolButton { text-align: left; font-weight: 600; padding: 3px; }");
    std::string key = std::string(collapsedPrefs) + "/" + title.toStdString();
    bool open = !PrefStore::get().value<bool>("general", key, !openByDefault);
    header->setChecked(open);
    header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    content->setParent(box);
    content->setVisible(open);
    QObject::connect(header, &QToolButton::toggled, content, [header, content, key](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        PrefStore::get().set("general", key, !on);
    });
    v->addWidget(header);
    v->addWidget(content, 1);
    // Keeps a collapsed header at the top when the section is given extra height.
    v->addStretch(0);
    if (headerOut)
        *headerOut = header;
    return box;
}
} // namespace lila::ui
