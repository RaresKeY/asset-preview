#pragma once
#include <f3d/scene.h>
#include <f3d/options.h>
#ifdef ASSET_PREVIEW_F3D4
#include <f3d/animation.h>
#endif

// Keep the packaged 3.5 backend and the workstation's 4.x public API usable.
namespace preview::f3dCompat {
inline unsigned animationCount(f3d::scene& scene) {
#ifdef ASSET_PREVIEW_F3D4
    return scene.getAnimation().count();
#else
    return scene.availableAnimations();
#endif
}
inline std::string animationName(f3d::scene& scene, unsigned index) {
#ifdef ASSET_PREVIEW_F3D4
    return scene.getAnimation().getName(int(index));
#else
    return scene.getAnimationName(index);
#endif
}
inline void loadTime(f3d::scene& scene, double time) {
#ifdef ASSET_PREVIEW_F3D4
    scene.getAnimation().loadTime(time);
#else
    scene.loadAnimationTime(time);
#endif
}
inline std::pair<double,double> timeRange(f3d::scene& scene) {
#ifdef ASSET_PREVIEW_F3D4
    return scene.getAnimation().getTimeRange();
#else
    return scene.animationTimeRange();
#endif
}
inline bool supports(f3d::scene& scene, const std::filesystem::path& path) {
#ifdef ASSET_PREVIEW_F3D4
    return scene.supports(path)==f3d::file_availability::SUPPORTED;
#else
    return scene.supports(path);
#endif
}
inline void antialias(f3d::options& options) {
#ifdef ASSET_PREVIEW_F3D4
    options.render.effect.antialiasing.mode="fxaa";
#else
    options.render.effect.antialiasing.enable=true;
#endif
}
}
