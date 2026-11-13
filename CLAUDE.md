# UE Plugin Instructions

Editor-only plugin (all modules are `Type: Editor` in the .uplugin). It never ships in packaged games.
- Editor dependencies (UnrealEd, Slate, EditorSubsystem, etc.) are fine; no `WITH_EDITOR` guards needed.
- Don't add Runtime modules or change module types without asking.

## Environment
- Engine: `D:/Gamedev/SCUMGame/Engine` (licensee UE 4.27.2 source build; compile as C++14, no C++17 features)
- Host project: the `.uproject` two folders above this file (`../../*.uproject`).
  This plugin lives in that project's `Plugins/` folder, inside a Perforce workspace: never run P4 operations, and never change the `.uproject`.
- Build: `"D:/Gamedev/SCUMGame/Engine/Build/BatchFiles/Build.bat" SCUMEditor Win64 Development -Project="D:/Gamedev/SCUMGame/SCUM/SCUM.uproject" -WaitMutex`
  The editor is not built locally, so the user builds it and reports errors.
- After C++ changes, build and fix errors before finishing. If building isn't possible, verify edits didn't alter declarations, macros, includes, or signatures, and check every engine API against the 4.27 headers.

## Scope
- Change only what the task needs; suggest unrelated improvements instead of making them.

## Style
- Prefer simple, readable code with descriptive names over clever or abstract solutions.
- Don't extract a function for one or two trivial lines; do extract for reuse, naming a complex idea, isolating UE setup, or keeping a long function readable.

## Comments
- Headers: short Unreal-style `/** */` comment on every property, function, delegate, struct, enum, and public API. State purpose, not types.
- `.cpp`: comment non-trivial functions with purpose, inputs, and outputs/side effects. Skip trivial getters/setters.
- Preserve existing comments unless wrong; update comments when behavior changes.

```cpp
/** Starts watching the selected stat group. */
UFUNCTION(BlueprintCallable, Category="Stats")
void StartWatching(FName StatGroup);
```

## Unreal
- Follow UE naming, reflection macros, and tab indentation.
- Don't change metadata, categories, access modifiers, or API macros unless required.
- Keep `.generated.h` as the last include.
- Add redirects when renaming reflected types or properties.
- Never edit `Binaries/`, `Intermediate/`, or engine source.