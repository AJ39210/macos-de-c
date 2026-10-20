#!/bin/sh
set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo "Run this user installer as your regular user, without sudo." >&2
    exit 1
fi

: "${HOME:?HOME must be set}"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
bin_dir="$HOME/.local/bin"
case "$data_home" in
    /*) ;;
    *)
        echo "XDG_DATA_HOME must be an absolute path." >&2
        exit 1
        ;;
esac
if [ "$data_home" = / ]; then
    echo "Refusing to install into the filesystem root." >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
install_dir="$data_home/macos-de"
session_dir="$data_home/xsessions"
applications_dir="$data_home/applications"
session_link="$HOME/.local/bin/macos-de-session"

for required in bin assets css apps applications widgets; do
    if [ ! -d "$build_dir/$required" ]; then
        echo "Missing build output: $build_dir/$required (run 'make' first)." >&2
        exit 1
    fi
done

desktop_entry="$build_dir/sessions/macos-de.desktop"
if [ ! -f "$desktop_entry" ]; then
    echo "Missing generated desktop entry: $desktop_entry (run 'make' first)." >&2
    exit 1
fi

mkdir -p "$install_dir" "$session_dir" "$applications_dir" "$bin_dir"
if [ -e "$session_link" ] || [ -L "$session_link" ]; then
    if [ ! -L "$session_link" ] ||
       [ "$(readlink "$session_link")" != "$install_dir/bin/macos-de-session-local" ]; then
        echo "Refusing to replace existing file: $session_link" >&2
        exit 1
    fi
fi
cp -a "$build_dir/bin" "$build_dir/assets" "$build_dir/css" \
    "$build_dir/apps" "$build_dir/widgets" "$install_dir/"

cat > "$install_dir/bin/macos-de-session-local" <<EOF
#!/bin/sh
set -eu
script_dir=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
install_dir=\$(CDPATH= cd -- "\$script_dir/.." && pwd)
cd "\$install_dir"
exec "\$install_dir/bin/macos-de-session" "\$@"
EOF
chmod 755 "$install_dir/bin/macos-de-session-local"

escape_sed_replacement() {
    printf '%s' "$1" | sed 's/[\\&|]/\\&/g'
}

escaped_install_dir=$(escape_sed_replacement "$install_dir")
escaped_exec_dir=$(escape_sed_replacement "$install_dir" | sed 's/ /\\\\ /g')
sed \
    -e "s|^Exec=/usr/share/macos-de/bin/macos-de-session$|Exec=$escaped_exec_dir/bin/macos-de-session-local|" \
    -e "s|/usr/share/macos-de|$escaped_install_dir|g" \
    "$desktop_entry" > "$session_dir/macos-de.desktop"
chmod 644 "$session_dir/macos-de.desktop"

for desktop in "$build_dir"/applications/*.desktop; do
    if [ ! -f "$desktop" ]; then
        echo "No generated application desktop entries found in $build_dir/applications." >&2
        exit 1
    fi
    output="$applications_dir/$(basename "$desktop")"
    sed \
        -e "s|^Exec=/usr/share/macos-de/|Exec=$escaped_exec_dir/|" \
        -e "s|/usr/share/macos-de|$escaped_install_dir|g" \
        "$desktop" > "$output"
    chmod 644 "$output"
done

ln -s "$install_dir/bin/macos-de-session-local" "$session_link"

echo "macOS-DE installed for this user in $install_dir."
echo "Session and application entries were added under $data_home."
echo "If your display manager does not list user sessions, start it from an X session with: $bin_dir/macos-de-session"
echo "System PAM and backlight/udev permissions were not changed; those require an administrator."
