{
  lib,
  stdenv,
  rustPlatform,
  pkg-config,
  systemd,
  src,
}:

rustPlatform.buildRustPackage {
  pname = "mc-uart-bridge";
  version = "0.1.0";

  inherit src;
  cargoRoot = "bridge";
  buildAndTestSubdir = "bridge";

  cargoHash = "sha256-dyko0T7npT1AzIgrzuzha8XHXGxXY2px/DFSeeRiYgg=";

  nativeBuildInputs = lib.optionals stdenv.hostPlatform.isLinux [
    pkg-config
  ];

  buildInputs = lib.optionals stdenv.hostPlatform.isLinux [
    systemd
  ];

  meta = {
    description = "TCP to UART bridge for MC UART server";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux ++ lib.platforms.windows;
  };
}
