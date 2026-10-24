#pragma once

#include "core/Export.h"

#include <QStringList>
#include <QWidget>

#include <memory>
#include <string>

class QComboBox;
class QLineEdit;
class QListWidget;
class QTreeWidget;

namespace lila::ui
{
inline constexpr const char* kMaterialBrowserDock = "materialBrowser";
/** Drag payload of a library material: its id, UTF-8. Viewports take it (applyLibraryMaterial); a layer stack can too. */
inline constexpr const char* kMaterialMime = "application/x-lila-material";

/**
 * Registers the Material Browser panel (window.materialBrowser), the material.* actions, the viewport drop of library
 * materials, and adds them to the Texture menu.
 */
LILA_UI_API void registerMaterialBrowserUi();

/**
 * The material library as a searchable thumbnail grid: a search field, a category and tag tree, favourites, a preset
 * choice for the selected material. Double-click applies to the selection; drag onto a mesh or face selection applies there.
 */
class LILA_UI_API MaterialBrowser : public QWidget
{
public:
    explicit MaterialBrowser(QWidget* parent = nullptr);
    ~MaterialBrowser() override;

    /** Rereads the library and refills the grid with the current filters. */
    void refresh();
    /** Ids shown in the grid, in order. */
    QStringList shownIds() const;
    /** The selected material's id, empty for none. */
    std::string selectedId() const;
    void select(const std::string& id);
    /** The preset chosen for the selected material; empty is its defaults. */
    std::string preset() const;
    /** applyLibraryMaterial on the selection with the chosen preset. */
    void applySelected();

    QLineEdit* searchField() const;
    QTreeWidget* filterTree() const;
    QListWidget* grid() const;
    QComboBox* presetBox() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

/** The preset the browser has chosen for a material (drops use it); empty for its defaults. */
LILA_UI_API std::string chosenPreset(const std::string& id);
} // namespace lila::ui
