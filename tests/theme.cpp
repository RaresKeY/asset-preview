#include "theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

static double luminance(const QColor& color) {
    const auto linear=[](double value) { return value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4); };
    return .2126*linear(color.redF())+.7152*linear(color.greenF())+.0722*linear(color.blueF());
}
static double contrast(const QColor& first,const QColor& second) {
    auto a=luminance(first),b=luminance(second);
    return (std::max(a,b)+.05)/(std::min(a,b)+.05);
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    const auto original=app.palette();
    for (const auto& mode : {QString("dark"),QString("light")}) {
        Theme::apply(mode);
        const auto palette=app.palette();
        if (Theme::effective()!=mode || !app.testAttribute(Qt::AA_SetPalette)) return 1;
        if (QToolTip::palette()!=palette) return 4;
        for (const auto group : {QPalette::Active,QPalette::Inactive})
            for (const auto& roles : {std::pair{QPalette::WindowText,QPalette::Window},
                                     std::pair{QPalette::Text,QPalette::Base},
                                     std::pair{QPalette::ButtonText,QPalette::Button},
                                     std::pair{QPalette::HighlightedText,QPalette::Highlight},
                                     std::pair{QPalette::ToolTipText,QPalette::ToolTipBase},
                                     std::pair{QPalette::PlaceholderText,QPalette::Base}})
                if (contrast(palette.color(group,roles.first),palette.color(group,roles.second))<4.5) return 2;
        Theme::apply("auto");
        if (app.testAttribute(Qt::AA_SetPalette) || app.palette().resolveMask()!=0 || app.palette()!=original ||
            QToolTip::palette().resolveMask()!=0 || QToolTip::palette()!=original) return 3;
    }
    std::puts("Dark/light text, selection, placeholder and tooltip contrast passed; Auto clears palette overrides and restores platform colors.");
}
