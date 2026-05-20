.PHONY: firmware firmware-legacy firmware-aggressive firmware-log-debug test-native bridge check clean

firmware:
	nix build .#firmware-c2

firmware-legacy:
	nix build .#firmware-c2-legacy

firmware-aggressive:
	nix build .#firmware-c2-aggressive

firmware-log-debug:
	nix build .#log-debug

test-native:
	nix build .#native-tests

bridge:
	nix build .#bridge

check:
	nix flake check

clean:
	rm -rf build .ecos-build result result-* bridge/target
