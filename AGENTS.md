## Build

Build only with `msbuild AGShot.sln /p:Configuration=Debug /p:Platform=x64`.
Never build a `.vcxproj` directly.
Never edit `AGShot.vcxproj`, `AGShot.sln`, `PropertySheet.props` or any other project file, merely for temporary preprocessor definitions. Not temporarily, not "just to test something", not reverted afterwards.
If a build variant needs an extra preprocessor definition, inject a props file with /p:ForceImportBeforeCppTargets=<path to props> instead.

## Comments and TODOs

A TODO is one paragraph. Never split it across paragraphs with blank comment lines.

## Deletion

Never perform any real deletion; everything removed goes to the Recycle Bin.

## Forbidden

Never use subagents.
