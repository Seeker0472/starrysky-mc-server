{
  lib,
  stdenvNoCC,
  src,
}:

stdenvNoCC.mkDerivation {
  pname = "ecos-sdk";
  version = "unstable";

  inherit src;

  patches = [
    ./patches/ecos-sdk-nix-backend.patch
  ];

  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp -R . "$out/"
    runHook postInstall
  '';

  meta = {
    description = "Patched ECOS embedded SDK for MC UART firmware builds";
    license = lib.licenses.asl20;
    platforms = lib.platforms.linux;
  };
}
