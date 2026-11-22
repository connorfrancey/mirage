# C++ API

[Open the searchable Doxygen reference](../cpp-reference/index.html) for the native mod headers and loader interfaces. It includes [API groups](../cpp-reference/topics.html) and [source files](../cpp-reference/files.html).

The [C ABI function table](../api-reference.md) is the mod compatibility boundary. Native C++ helpers in `mirage/mirage.hpp` compile into the mod. Loader interfaces appear in the reference to support contributors.

Engine objects belong to the game thread. Resolve them during the current tick and reacquire them after a load. Draw through the render-thread callbacks. A mod can retain its own data between callbacks. Release subscriptions, hooks, and other registrations when the owning module is disabled or unloaded.
