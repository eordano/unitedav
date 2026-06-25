# Compile-checking the C# package outside Unity

The package targets Unity, but we can catch syntax/signature/P-Invoke errors
without Unity by compiling against **throwaway** stubs of the UnityEngine APIs we
use. The stubs live OUTSIDE this package (so they never collide with real Unity).

## Repro (mono / mcs, provided by Nix)

```sh
# from the repo root (unitedav/)
nix --extra-experimental-features 'nix-command flakes' shell nixpkgs#mono -c bash -c '
  mcs -target:library -unsafe -langversion:latest \
    -out:/tmp/UnitedAV.Runtime.dll \
    -recurse:"unity/Runtime/UnitedAV/*.cs" \
    /tmp/unity-stubs/UnityStubs.cs
'
```

- `-langversion:latest` is required: the code uses C# 7.1+ features (`default`
  literal, etc.); Unity 2021+/`unity-explorer` uses C# 9, so this is correct.
- `-unsafe`: the interop uses pointers/`IntPtr` for frame data.
- `/tmp/unity-stubs/UnityStubs.cs`: minimal stand-ins for `MonoBehaviour`,
  `Texture2D`, `AudioSource`, `AudioClip`, `UnityEvent<,,>`, etc. **Not** shipped.

## Expected result

`Compilation succeeded` with zero warnings. (`MediaPlayerEvent.EventType.ReadyToPlay`
is fired from `PollStateAndFireEvents()`, gated once per open by `_metaDataFired`.)

## Real verification

This only proves it *compiles*. True verification = compile a real consumer's
media module against this package and run it in Unity.
