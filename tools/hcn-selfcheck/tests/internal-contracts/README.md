# HCN Internal contract tests

From this directory, build this standalone x64 test project using Visual Studio
2022 with the v143 toolset:

```powershell
msbuild .\hcn-internal-contract-tests.vcxproj /p:Configuration=Debug /p:Platform=x64
.\bin\x64\Debug\hcn-internal-contract-tests.exe
```

The test links the production owned-delete core and Internal/External
wrappers, census authority gate, final cancellation check, classifier, and
resolver. It replaces HCN and WMI calls with local stubs, assigns a fake
`pfnDeleteNet`, and never loads `computenetwork.dll` or mutates host network
state. The External ordinary-failure diagnostic may read `GetIfTable2` to
format an adapter description; it does not change adapter or network state.

The assertions cover Internal fixed-ID deletion and refusal of borrowed,
foreign, and zero IDs; External derived-ID deletion, exact delete-not-found,
ordinary failure, and missing-export behavior; lock ownership during the
actual delete call; filtered-token empty-view refusal; cancellation raised
inside the final fake HCN probe; separate UI-census and explicit-acquire WMI
deadline policies; cleanup after session-open, topology, and partially
allocated census failures; and projection of resolver reasons while
preserving E3 rejection text and keeping `deferEligible` PRIVATE-only.
