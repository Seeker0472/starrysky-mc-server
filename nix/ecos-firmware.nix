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
  python3,
  riscvToolchain,
  sdk,
  src,
  board,
  defconfig,
  firmwareName ? "mc_uart_fw",
  variant ? "",
  extraCFlags ? "",
  compressedMapAssets ? false,
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
  ] ++ lib.optional compressedMapAssets python3;

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    export ECOS_SDK_HOME="${sdk}"
    psramRuntimeBytes=0
  '' + lib.optionalString compressedMapAssets ''
    python3 scripts/generate_compressed_chunks.py --out-dir core/generated --check
    psramRuntimeBytes="$(sed -n 's/^const uint32_t mc_world_compressed_total_bytes = \([0-9][0-9]*\)u;$/\1/p' core/generated/mc_world_compressed_assets.c)"
    if [ -z "$psramRuntimeBytes" ]; then
      echo "failed to extract mc_world_compressed_total_bytes" >&2
      exit 1
    fi
  '' + ''
    make -f "${sdk}/mk/ecos-firmware.mk" \
      PROJECT_DIR="$PWD" \
      BUILD_DIR="$PWD/build" \
      CONFIG_DIR="$PWD/.ecos-build/config" \
      DEFCONFIG="${defconfig}" \
      BOARD="${board}" \
      CROSS="riscv64-none-elf-" \
      FIRMWARE_NAME="${firmwareName}" \
      COMPRESSED_MAP_ASSETS="${if compressedMapAssets then "1" else "0"}" \
      PSRAM_RUNTIME_BYTES="$psramRuntimeBytes" \
      EXTRA_CFLAGS="${extraCFlags}"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    test -f "build/${firmwareName}"
    test -f "build/${firmwareName}.bin"
    test -f "build/${firmwareName}.hex"
    if [ ! -f "build/memory-report.txt" ]; then
      echo "missing firmware memory report: build/memory-report.txt" >&2
      exit 1
    fi
    if [ ! -f "build/${firmwareName}.txt" ]; then
      riscv64-none-elf-objdump -d "build/${firmwareName}" > "build/${firmwareName}.txt"
    fi
    test -f "build/${firmwareName}.txt"
    install -Dm755 "build/${firmwareName}" "$out/${firmwareName}"
    cp "build/${firmwareName}" "$out/${firmwareName}.elf"
    install -Dm644 "build/${firmwareName}.bin" "$out/${firmwareName}.bin"
    install -Dm644 "build/${firmwareName}.hex" "$out/${firmwareName}.hex"
    install -Dm644 "build/${firmwareName}.txt" "$out/${firmwareName}.txt"
    install -Dm644 "build/memory-report.txt" "$out/memory-report.txt"
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
