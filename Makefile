.PHONY: firmware firmware-reversed test-native bridge check clean

firmware:
	nix build .#firmware-c2

firmware-reversed:
	nix build .#firmware-c2-uart-reversed

test-native:
	nix build .#native-tests

bridge:
	nix build .#bridge

check:
	nix flake check

clean:
	rm -rf build .ecos-build result result-* bridge/target
