#pragma once
#include <QApplication>
#include <QPalette>
#include <QString>
#include <QToolTip>

namespace Theme {
inline bool valid(const QString& mode) {
    return mode=="auto" || mode=="dark" || mode=="light";
}

inline QString effective() {
    return QApplication::palette().color(QPalette::Window).lightness()<128 ? "dark" : "light";
}

inline void apply(const QString& mode) {
    // An unresolved palette removes application overrides, so Auto follows the
    // current platform palette and subsequent system changes without polling.
    QPalette palette;
    if (mode=="auto") {
        QApplication::setPalette(palette);
        QToolTip::setPalette(QApplication::palette());
        return;
    }
    const bool dark=mode=="dark";
    const QColor foreground(dark ? "#eff0f1" : "#232629");
    const QColor muted(dark ? "#9da7af" : "#687279");
    palette.setColor(QPalette::Window, QColor(dark ? "#232629" : "#eff0f1"));
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::Base, QColor(dark ? "#1b1e20" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor(dark ? "#292e32" : "#f6f7f7"));
    palette.setColor(QPalette::Text, foreground);
    palette.setColor(QPalette::Button, QColor(dark ? "#31363b" : "#eff0f1"));
    palette.setColor(QPalette::ButtonText, foreground);
    palette.setColor(QPalette::BrightText, QColor(dark ? "#ffffff" : "#a51c30"));
    palette.setColor(QPalette::Light, QColor(dark ? "#56616a" : "#ffffff"));
    palette.setColor(QPalette::Midlight, QColor(dark ? "#414950" : "#e2e5e7"));
    palette.setColor(QPalette::Mid, QColor(dark ? "#4d565e" : "#a4acb2"));
    palette.setColor(QPalette::Dark, QColor(dark ? "#141719" : "#7d878f"));
    palette.setColor(QPalette::Shadow, QColor(dark ? "#090b0c" : "#535c63"));
    palette.setColor(QPalette::Highlight, QColor(dark ? "#2b6f9d" : "#22689d"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(dark ? "#6ab9eb" : "#145d99"));
    palette.setColor(QPalette::LinkVisited, QColor(dark ? "#c59be8" : "#6d3c91"));
    palette.setColor(QPalette::ToolTipBase, QColor(dark ? "#31363b" : "#ffffdc"));
    palette.setColor(QPalette::ToolTipText, foreground);
    palette.setColor(QPalette::PlaceholderText, QColor(dark ? "#b0b7bd" : "#626b72"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::Accent, QColor(dark ? "#3daee9" : "#22689d"));
#endif
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                            QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, muted);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(dark ? "#40464b" : "#d7dbde"));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, muted);
    QApplication::setPalette(palette);
    QToolTip::setPalette(QApplication::palette());
}
}
