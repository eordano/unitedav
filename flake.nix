# SPDX-License-Identifier: Apache-2.0
{
  description = "UnitedAV — an FFmpeg/LGPL audio/video plugin for Unity";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems
        (system: f (import nixpkgs { inherit system; }));

      mkFfmpeg = pkgs: pkgs.ffmpeg.override {
        withGPL = false;
        withGPLv3 = false;
        withUnfree = false;
      };

      vaapiInputs = pkgs:
        nixpkgs.lib.optionals (pkgs.stdenv.isLinux && pkgs.stdenv.hostPlatform.isx86_64) [
          pkgs.libva pkgs.libva-utils pkgs.libdrm pkgs.intel-media-driver
        ];

      glInputs = pkgs:
        nixpkgs.lib.optionals pkgs.stdenv.isLinux [
          pkgs.libGL pkgs.mesa pkgs.libgbm pkgs.libdrm pkgs.libva
        ];
      vkInputs = pkgs:
        nixpkgs.lib.optionals pkgs.stdenv.isLinux [
          pkgs.vulkan-loader pkgs.vulkan-headers pkgs.shaderc
        ];
      vaapiHook = pkgs:
        if (pkgs.stdenv.isLinux && pkgs.stdenv.hostPlatform.isx86_64) then ''
          export LIBVA_DRIVER_NAME=iHD
          export LIBVA_DRIVERS_PATH=${pkgs.intel-media-driver}/lib/dri
        '' else "";
    in
    {
      apps = forAllSystems (pkgs: {
        mediamtx = {
          type = "app";
          program = "${pkgs.mediamtx}/bin/mediamtx";
        };
      });

      packages = forAllSystems (pkgs:
        let
          ffmpeg = mkFfmpeg pkgs;
          common = {
            pname = "unitedav"; version = "0.0.0"; src = ./native;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          };
        in {
          default = pkgs.stdenv.mkDerivation (common // {
            buildInputs = [ ffmpeg ] ++ vaapiInputs pkgs ++ glInputs pkgs;
            cmakeFlags = [ "-DUAV_SKIP_PLUGIN_COPY=ON" "-DUAV_ENABLE_GPU=ON" ];
          });
          ffmpeg-lgpl = ffmpeg;
        }
        // nixpkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          vulkan = pkgs.stdenv.mkDerivation (common // {
            pname = "unitedav-vulkan";
            buildInputs = [ ffmpeg ] ++ vaapiInputs pkgs ++ glInputs pkgs ++ vkInputs pkgs;
            cmakeFlags = [ "-DUAV_SKIP_PLUGIN_COPY=ON" "-DUAV_ENABLE_GPU=ON" "-DUAV_GPU_API=vulkan" ];
          });
        });

      devShells = forAllSystems (pkgs:
        let ffmpeg = mkFfmpeg pkgs; in {
          default = pkgs.mkShell {
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.gdb pkgs.clang-tools pkgs.mediamtx
                                  pkgs.doctest pkgs.clang pkgs.llvm ]
              ++ nixpkgs.lib.optionals pkgs.stdenv.isLinux [
                pkgs.valgrind
                pkgs.obs-studio pkgs.xvfb-run pkgs.obs-cmd pkgs.websocat
              ];
            buildInputs = [ ffmpeg ] ++ vaapiInputs pkgs ++ glInputs pkgs ++ vkInputs pkgs;
            shellHook = (vaapiHook pkgs) + ''
              echo "UnitedAV dev shell"
              echo "  ffmpeg:  $(pkg-config --modversion libavcodec 2>/dev/null || echo missing) (libavcodec)"
              echo "  cmake:   $(cmake --version | head -1)"
              echo "  mediamtx: $(mediamtx --version 2>/dev/null || echo missing)"
              ${nixpkgs.lib.optionalString (pkgs.stdenv.isLinux && pkgs.stdenv.hostPlatform.isx86_64)
                ''echo "  vaapi:   $LIBVA_DRIVERS_PATH (LIBVA_DRIVER_NAME=$LIBVA_DRIVER_NAME)"''}
              echo "  build:   cmake -S native -B native/build -G Ninja && cmake --build native/build"
            '';
          };
        });
    };
}
