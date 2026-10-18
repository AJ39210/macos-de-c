# Making a macOS-DE widget package

macOS-DE supports third-party widgets as `.widg` files. A `.widg` file is a ZIP
archive containing a prebuilt Linux executable, its `config.cfg`, and any
assets it needs. The desktop starts the executable as your user when the widget
is enabled; the executable is not sandboxed.

## Package layout

```text
my-widget/
├── config.cfg
├── bin/
│   └── my-widget
├── style.css
└── assets/
    └── icon.png
```

`config.cfg` must contain a `[Widget]` section:

```ini
[Widget]
name=My Widget
id=my-widget
exec=bin/my-widget
style=style.css
version=1
```

`id` may contain only ASCII letters, digits, hyphens, and underscores. It must
be unique among the widgets installed for that user. `exec` is a relative path
to an executable file inside the package. The optional `style` and `version`
keys are descriptive; the widget application is responsible for its own GTK
window, drawing, and styling.

## Build and install

1. Write the widget as a Linux application that opens and manages its own GTK
   window. The DE launches it as a separate process with the package folder as
   its working directory. Use relative paths from that folder for bundled
   assets.
2. Build the executable for the target machine's architecture and runtime
   libraries. Set its executable bit (for example, `chmod +x bin/my-widget`).
3. Put `config.cfg`, the executable, and required assets in one package folder.
   The config can either be at the archive root or inside one top-level folder.
4. Create a ZIP archive while preserving Unix executable permissions, then
   rename it to `.widg`, for example:

   ```sh
   zip -r my-widget.zip my-widget/
   mv my-widget.zip my-widget.widg
   ```

5. In DE Settings, open **Wallpaper And Widgets**, choose **Add Widget…**, and
   select the `.widg` file. Enable it from its row to launch it. Turn it off to
   stop it, or choose **Remove** to unload and delete the installed package.

The package importer rejects unsafe paths, links/special files, archives with
more than 512 entries, and archives that expand beyond 256 MiB. Only install
widget executables you trust: they run with your account's permissions and
have full access to your files.

## Built-in System Stats

The System Stats widget is built into macOS-DE and remains independently
available from the Widgets section in DE Settings. It does not need to be
packaged or installed as a `.widg` file.
