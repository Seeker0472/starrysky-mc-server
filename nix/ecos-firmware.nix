{
  lib,
  stdenvNoCC,
  bison,
  coreutils,
  findutils,
  flex,
  gawk,
  gcc,
  gnumake,
  gnused,
  gnugrep,
  riscvToolchain,
  sdk,
  src,
  board,
  defconfig,
  firmwareName ? "mc_uart_fw",
  variant ? "",
  extraCFlags ? "",
}:

stdenvNoCC.mkDerivation {
  pname = "mc-uart-${board}-firmware" + lib.optionalString (variant != "") "-${variant}";
  version = "0.1.0";

  inherit src;

  nativeBuildInputs = [
    bison
    coreutils
    findutils
    flex
    gawk
    gcc
    gnumake
    gnused
    gnugrep
    riscvToolchain
  ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    export ECOS_SDK_HOME="${sdk}"
    make -f "${sdk}/mk/ecos-firmware.mk" \
      PROJECT_DIR="$PWD" \
      BUILD_DIR="$PWD/build" \
      CONFIG_DIR="$PWD/.ecos-build/config" \
      DEFCONFIG="${defconfig}" \
      BOARD="${board}" \
      CROSS="riscv64-none-elf-" \
      FIRMWARE_NAME="${firmwareName}" \
      EXTRA_CFLAGS="${extraCFlags}"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    test -f "build/${firmwareName}"
    test -f "build/${firmwareName}.bin"
    test -f "build/${firmwareName}.hex"
    test -f "build/${firmwareName}.txt"
    install -Dm755 "build/${firmwareName}" "$out/${firmwareName}"
    cp "build/${firmwareName}" "$out/${firmwareName}.elf"
    install -Dm644 "build/${firmwareName}.bin" "$out/${firmwareName}.bin"
    install -Dm644 "build/${firmwareName}.hex" "$out/${firmwareName}.hex"
    install -Dm644 "build/${firmwareName}.txt" "$out/${firmwareName}.txt"
    if [ -f "build/${firmwareName}.map" ]; then
      install -Dm644 "build/${firmwareName}.map" "$out/${firmwareName}.map"
    fi
    runHook postInstall
  '';

  meta = {
    description = "MC UART firmware for ${board}";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
  };
}
