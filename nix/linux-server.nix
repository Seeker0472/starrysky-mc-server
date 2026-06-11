{
  stdenv,
  python3,
  src,
}:

stdenv.mkDerivation {
  pname = "mc-linux-server";
  version = "0.1.0";

  inherit src;

  dontConfigure = true;

  nativeBuildInputs = [
    python3
  ];

  buildPhase = ''
    runHook preBuild
    python3 scripts/generate_compressed_chunks.py --out-dir core/generated --check
    mkdir -p build/linux
    cc -std=c11 -Wall -Wextra -Werror -O2 \
      -DMC_PROTOCOL_COMPRESSION_ENABLE=1 \
      -DMC_USE_PSRAM_COMPRESSED_MAP=1 \
      -DMC_COMPRESSION_THRESHOLD=8192u \
      -Icore/include \
      -Icore/generated \
      linux/mc_linux_server.c \
      core/src/mc_ringbuf.c \
      core/src/mc_varint.c \
      core/src/mc_packet.c \
      core/src/mc_commands.c \
      core/src/mc_world.c \
      core/src/mc_world_compressed.c \
      core/generated/mc_world_compressed_assets.c \
      core/src/mc_server.c \
      -o build/linux/mc-linux-server
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin"
    install -Dm755 build/linux/mc-linux-server "$out/bin/mc-linux-server"
    runHook postInstall
  '';

  meta.description = "Linux TCP direct entry point for the MC UART core server";
}
