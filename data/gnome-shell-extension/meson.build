# The GNOME Shell extension that places the quick terminal on Wayland

extension_uuid = 'quick-terminal@pano.aripd.com'
extension_dir = gt_datadir / 'gnome-shell' / 'extensions' / extension_uuid

configure_file(
  input: 'metadata.json.in',
  output: 'metadata.json',
  configuration: {'uuid': extension_uuid},
  install: true,
  install_dir: extension_dir,
  install_tag: 'runtime',
)

install_data(
  'extension.js',
  install_dir: extension_dir,
  install_tag: 'runtime',
)
