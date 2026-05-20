{
  description = "Nix-first Minecraft UART server for StarrySky C2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    embedded-sdk = {
      url = "git+file:../embedded-sdk";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, embedded-sdk }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      defaultBoard = "c2";
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          lib = pkgs.lib;
          boards = import ./nix/boards.nix;

          projectSrc = lib.cleanSourceWith {
            src = ./.;
            filter = path: type:
              let base = baseNameOf path;
              in !(base == "result"
                || lib.hasPrefix "result-" base
                || base == ".direnv"
                || base == ".git"
                || base == ".ecos-build"
                || base == "build"
                || base == "target");
          };

          sdk = pkgs.callPackage ./nix/ecos-sdk.nix {
            src = embedded-sdk;
          };

          mkFirmware = boardKey: extraArgs:
            let boardConfig = boards.${boardKey};
            in pkgs.callPackage ./nix/ecos-firmware.nix ({
              inherit sdk;
              src = projectSrc;
              board = boardConfig.sdkBoard;
              defconfig = boardConfig.defconfig;
              firmwareName = boardConfig.firmwareName;
              riscvToolchain = pkgs.pkgsCross.riscv64-embedded.stdenv.cc;
            } // extraArgs);

          mkC2CompressedFlags = { logLevel ? "MC_LOG_INFO", aggressive ? false }:
            lib.concatStringsSep " " ([
              "-DMC_PSRAM_ENABLE=1"
              "-DMC_PROTOCOL_COMPRESSION_ENABLE=1"
              "-DMC_USE_PSRAM_COMPRESSED_MAP=1"
              "-DMC_COMPRESSION_THRESHOLD=8192u"
              "-DMC_LOG_LEVEL=${logLevel}"
            ] ++ lib.optional aggressive "-DMC_M2C_AGGRESSIVE_TX=1");

          firmwareC2 = mkFirmware "c2" {
            compressedMapAssets = true;
            extraCFlags = mkC2CompressedFlags {};
          };
        in
        {
          firmware-c2 = firmwareC2;
          firmware-c2-legacy = firmwareC2.override {
            variant = "legacy";
            compressedMapAssets = false;
            extraCFlags = "";
          };
          firmware-c2-aggressive = firmwareC2.override {
            variant = "aggressive";
            compressedMapAssets = true;
            extraCFlags = mkC2CompressedFlags { aggressive = true; };
          };
          log-debug = firmwareC2.override {
            variant = "log-debug";
            compressedMapAssets = true;
            extraCFlags = mkC2CompressedFlags { logLevel = "MC_LOG_DEBUG"; };
          };
          ecos-sdk = sdk;
          native-tests = pkgs.callPackage ./nix/native-tests.nix {
            src = projectSrc;
          };
          bridge = pkgs.callPackage ./nix/bridge.nix {
            src = projectSrc;
          };
          bridge-windows = pkgs.pkgsCross.mingwW64.callPackage ./nix/bridge.nix {
            src = projectSrc;
          };
          default = firmwareC2;
        });

      checks = forAllSystems (system: {
        native-tests = self.packages.${system}.native-tests;
        bridge = self.packages.${system}.bridge;
        bridge-windows = self.packages.${system}.bridge-windows;
        firmware-c2 = self.packages.${system}.firmware-c2;
        firmware-c2-legacy = self.packages.${system}.firmware-c2-legacy;
        firmware-c2-aggressive = self.packages.${system}.firmware-c2-aggressive;
        log-debug = self.packages.${system}.log-debug;
      });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          sdk = self.packages.${system}.ecos-sdk;
        in
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.bison
              pkgs.coreutils
              pkgs.findutils
              pkgs.flex
              pkgs.gawk
              pkgs.gcc
              pkgs.gnumake
              pkgs.gnused
              pkgs.gnugrep
              pkgs.rustc
              pkgs.cargo
              pkgs.pkg-config
              pkgs.systemd
              pkgs.pkgsCross.riscv64-embedded.stdenv.cc
            ];

            ECOS_SDK_HOME = "${sdk}";
            BOARD = defaultBoard;
            CROSS = "riscv64-none-elf-";
            DEFCONFIG = "configs/c2_defconfig";
            FIRMWARE_NAME = "mc_uart_fw";

            shellHook = ''
              echo "ECOS_SDK_HOME=$ECOS_SDK_HOME"
              echo "Run: make test-native"
            '';
          };
        });
    };
}
