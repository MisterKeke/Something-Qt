# Assets

The bundled `appicon.png`, retained navigation illustrations, and the Sandrone, Hu Tao, and Skirk wallpaper images were copied from `D:\Projects\Something-v2\frontend\dist\assets` into this independent project. The reference files remain untouched. They are registered as Qt resources by the root CMake configuration.

Imported user wallpapers are copied at runtime into `<Qt AppLocalDataLocation>/profiles/<profile-id>/assets/wallpapers/`; they are not committed here. The built-in Original option uses the dark shell background and accent glow rather than an image file.

Character navigation images use WebP. The inspected Qt kit had no `qwebp` image-format plugin, so those images may fall back to their QML glyphs until a WebP-capable runtime is selected. This does not change the imported-file validation limit or bypass Qt's image decoder.
