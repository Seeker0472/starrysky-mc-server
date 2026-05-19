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

          firmwarePackages = lib.mapAttrs' (boardKey: _boardConfig:
            lib.nameValuePair "firmware-${boardKey}" (mkFirmware boardKey {})
          ) boards;
        in
        firmwarePackages // {
          firmware-c2-uart-reversed = mkFirmware "c2" {
            variant = "uart-reversed";
            extraCFlags = "-DMC_BRIDGE_UART_ID=MC_UART_ID_1 -DMC_LOG_UART_ID=MC_UART_ID_0";
          };
          firmware-c2-aggressive = mkFirmware "c2" {
            variant = "aggressive";
            extraCFlags = "-DMC_M2C_AGGRESSIVE_TX=1";
          };
          log-debug = mkFirmware "c2" {
            variant = "log-debug";
            extraCFlags = "-DMC_BRIDGE_UART_ID=MC_UART_ID_0 -DMC_LOG_UART_ID=MC_UART_ID_1 -DMC_LOG_LEVEL=MC_LOG_DEBUG";
          };
          log-trace = mkFirmware "c2" {
            variant = "log-trace";
            extraCFlags = "-DMC_BRIDGE_UART_ID=MC_UART_ID_0 -DMC_LOG_UART_ID=MC_UART_ID_1 -DMC_LOG_LEVEL=MC_LOG_TRACE";
          };
          log-info = mkFirmware "c2" {
            variant = "log-info";
            extraCFlags = "-DMC_BRIDGE_UART_ID=MC_UART_ID_0 -DMC_LOG_UART_ID=MC_UART_ID_1 -DMC_LOG_LEVEL=MC_LOG_INFO";
          };
          log-none = mkFirmware "c2" {
            variant = "log-none";
            extraCFlags = "-DMC_BRIDGE_UART_ID=MC_UART_ID_0 -DMC_LOG_UART_ID=MC_UART_ID_1 -DMC_LOG_LEVEL=MC_LOG_OFF";
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
          default = firmwarePackages."firmware-${defaultBoard}";
        });

      checks = forAllSystems (system: {
        native-tests = self.packages.${system}.native-tests;
        bridge = self.packages.${system}.bridge;
        bridge-windows = self.packages.${system}.bridge-windows;
        firmware-c2 = self.packages.${system}.firmware-c2;
        firmware-c2-aggressive = self.packages.${system}.firmware-c2-aggressive;
        firmware-c2-uart-reversed = self.packages.${system}.firmware-c2-uart-reversed;
        log-debug = self.packages.${system}.log-debug;
        log-info = self.packages.${system}.log-info;
        log-none = self.packages.${system}.log-none;
        log-trace = self.packages.${system}.log-trace;
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
