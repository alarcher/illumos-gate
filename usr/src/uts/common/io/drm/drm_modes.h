extern struct drm_display_mode *drm_mode_duplicate(struct drm_device *dev,
					    const struct drm_display_mode *mode);

extern int drm_mode_vrefresh(const struct drm_display_mode *mode);
extern int drm_mode_hsync(const struct drm_display_mode *mode);
