#pragma once

#include "core/Export.h"

#include <QIcon>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;
class QToolButton;

namespace lila::scene
{
struct NodeType;
}

namespace lila::ui
{
/**
 * Images an image attr picks from: Lila's, the user's and the project's, each a folder. An attr is tied to one with
 * imagePicker::setLibraryFor; light.texture uses "hdri".
 */
namespace imagePicker
{
struct Entry
{
    /** What the attr stores: a bare file name for Lila's own (resolved by the attr's reader), else a path. */
    std::string value;
    QString name;
    std::filesystem::path file;
    QString group;
};

struct Library
{
    std::string id;
    /** "HDRIs": the add and open menu items name it. */
    QString title;
    std::function<std::vector<Entry>()> entries;
    std::filesystem::path userDir;
    /** Empty without an open project. */
    std::function<std::filesystem::path()> projectDir;
    QString filter;
};

/** Adds or replaces a library. "hdri" is built in. */
LILA_UI_API void addLibrary(Library library);
LILA_UI_API const Library* library(std::string_view id);
/** Ties nodeType.attr (and the same attr on derived types) to a library. */
LILA_UI_API void setLibraryFor(std::string nodeType, std::string attr, std::string library);
LILA_UI_API const Library* libraryFor(const scene::NodeType& type, std::string_view attr);

/** A thumbnail of an image file (HDRs tone mapped), size pixels on its long side; cached by file and modification time. Null when unreadable. */
LILA_UI_API QIcon thumbnail(const std::filesystem::path& file, int size = 48);
} // namespace imagePicker

/**
 * An image attr's editor, like the brush alpha picker: a dropdown of thumbnails (None, the library's images by folder,
 * the current file when it isn't one of them, Browse...) and a button to add images to the library or open its folders.
 */
class LILA_UI_API ImagePicker final : public QWidget
{
    Q_OBJECT
public:
    /** library may be null: then only None, the current file and Browse. */
    ImagePicker(const imagePicker::Library* library, QString title, QWidget* parent = nullptr);

    /** Shows value as the current choice without emitting chosen. */
    void setValue(const std::string& value);

Q_SIGNALS:
    void chosen(const QString& value);

private:
    void fill();
    void browse();
    void addTo(const std::filesystem::path& dir);

    const imagePicker::Library* mLibrary;
    QString mTitle;
    QComboBox* mCombo;
    std::string mValue;
};
} // namespace lila::ui
