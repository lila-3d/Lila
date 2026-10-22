#pragma once

#include <QColor>

/** The code editor's grayscale scheme. Tokens differ by lightness, weight and slant only, never hue. */
namespace lila::ui::palette
{
inline QColor gray(int v, int alpha = 255)
{
    return QColor(v, v, v, alpha);
}

inline const QColor background = gray(30);
inline const QColor text = gray(198);
inline const QColor currentLine = gray(38);
inline const QColor selection = gray(82);
inline const QColor gutterBg = gray(33);
inline const QColor gutterText = gray(92);
inline const QColor gutterCurrent = gray(190);
inline const QColor bracketMatch = gray(72);
inline const QColor searchMatch = gray(64);
inline const QColor searchCurrent = gray(100);
inline const QColor occurrence = gray(50);
inline const QColor cellRule = gray(74);
inline const QColor squiggleError = gray(235);
inline const QColor squiggleWarning = gray(150);
inline const QColor squiggleInfo = gray(105);
inline const QColor unnecessaryText = gray(112);
} // namespace lila::ui::palette
