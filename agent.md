# UE Plugin Coding Agent Instructions

## Scope Control

- Keep edits focused on what the user requested and what is necessary to complete it correctly.
- Suggest unrelated improvements instead of implementing them.

## Coding Style

- Prefer simple, straightforward code that is easy to read and understand.
- Use descriptive names and a clear control flow, even when the code could be made shorter.
- Avoid clever, overly abstract, or unnecessarily advanced solutions when a simpler approach works well.
- Introduce patterns, helpers, templates, or abstractions only when they provide a clear practical benefit.
- Keep the code correct and consistent with Unreal Engine conventions without making it needlessly complicated.

## Header Files (`.h`)

- Add a short, purpose-focused Unreal-style comment directly above properties, functions, delegates, structs, enums, and public APIs.
- Do not explain obvious types or implementation details.
- Preserve existing comments unless they are clearly wrong or outdated.

Example:

```cpp
/** Current number of tracked stat samples. */
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Stats")
int32 SampleCount = 0;

/** Starts watching the selected stat group. */
UFUNCTION(BlueprintCallable, Category="Stats")
void StartWatching(FName StatGroup);
```

## Source Files (`.cpp`)

- Add a concise comment immediately above each function definition that describes its purpose, relevant inputs, and return value or side effects.
- Mention missing inputs or outputs only when it improves clarity.
- Do not repeat implementation details that are clear from the function body.

Example:

```cpp
/**
 * Starts tracking the requested stat group.
 * Input: StatGroup identifies the Unreal stat group to monitor.
 * Output: Updates the watcher state and enables sampling.
 */
void FStatWatchModule::StartWatching(FName StatGroup)
{
    ActiveStatGroup = StatGroup;
    bIsWatching = true;
}

/**
 * Returns whether the watcher is currently sampling stats.
 * Output: True when stat sampling is active.
 */
bool FStatWatchModule::IsWatching() const
{
    return bIsWatching;
}
```

## General Commenting Rules

- Use short, natural language to document intent and behavior rather than repeating names or obvious code.
- Keep comments accurate when behavior changes.
- Use clear terms such as `Input`, `Output`, and `Returns` when they improve readability.

## Function Extraction

- Do not add a new function just to wrap one or two lines of code unless there is a good reason.
- Good reasons include reusing the same logic in multiple places, giving a complex idea a clear name, isolating Unreal-specific setup, improving testability, or keeping a larger function readable.
- If the extracted function only hides a simple constant, one direct call, or a tiny expression, prefer leaving the code inline.
- When a small helper already exists and has a clear purpose, keep it if it improves readability.


## Unreal Engine Conventions

- Respect Unreal naming and reflection conventions, including `UCLASS`, `USTRUCT`, `UENUM`, `UFUNCTION`, and `UPROPERTY`.
- Keep Blueprint-facing descriptions especially clear and compact.
- Do not change metadata, categories, access modifiers, or API macros unless the task specifically requires it.
- Preserve generated code includes and Unreal-required include order.

## Validation

- After editing plugin code, check that the project still compiles.
- If compilation is not available, at minimum verify that comments did not alter declarations, macros, includes, or function signatures.
