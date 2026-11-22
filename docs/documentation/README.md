# Building and maintaining the documentation

Install CMake 4.2 or newer, Python 3.12 or newer and Doxygen 1.9.8 or newer. Install Graphviz and put `dot` on `PATH` for dependency and collaboration diagrams. Doxygen uses its built-in class diagrams when Graphviz is unavailable.

The documentation-only configuration works on Windows and Linux. It skips the game toolchain and dependency downloads.

```sh
cmake -S . -B build/docs -D MIRAGE_DOCS_ONLY=ON
cmake --build build/docs --target docs-check
```

Generate the Material website using the same Python interpreter CMake detects. Install the requirements into a virtual environment first if you prefer to keep your global environment clean.

```sh
python -m pip install -r tools/documentation/requirements.txt
cmake -S . -B build/docs -D MIRAGE_DOCS_ONLY=ON -D MIRAGE_DOCS_WEBSITE=ON
cmake --build build/docs --target docs-site
python -m http.server 8000 --directory build/docs/documentation/site
```

Use `Python3_EXECUTABLE` to select the documentation environment if CMake finds a different interpreter. The `docs` target generates HTML and XML under `build/docs/documentation/doxygen`. `docs-check` uses XML to report coverage, and `docs-site` assembles the guides and references under `build/docs/documentation/site`. The site build runs MkDocs in strict mode. Doxygen checks documentation references.

For a normal loader configuration, enable `MIRAGE_BUILD_DOCS` and build the optional target through `uv run mirage build -t docs`. Set `MIRAGE_DOCS_HTML` or `MIRAGE_DOCS_XML` to `OFF` for a single output format. HTML is required for the website, and XML is required for coverage. Set `MIRAGE_DOCS_OUTPUT` to relocate the outputs.

The site includes the bundled MirageAPIGen runtime reference by default. Set `MIRAGE_DOCS_LUA=OFF` to omit it or use `MIRAGE_DOCS_LUA_CONFIG` and `MIRAGE_DOCS_LUA_INPUT` to build with another policy and metadata export. The optional inspection setting can add thousands of SDK pages.

`MIRAGE_DOCS_SDK=ON` writes to `doxygen-sdk` within the documentation output. This keeps the full UE3 reference separate from the default reference.

CI builds the site and uploads it with the XML and coverage report. The website and Doxygen use Mirage’s build version by default. Set `MIRAGE_DOCS_VERSION` for a tag or preview and publish the resulting site under that version’s directory when hosting multiple versions.

## Documenting an interface

Put documentation next to the declaration. Start with `@brief` for its purpose and use `@param` and `@return` to explain inputs and failures. Use `@note` for thread and lifetime contracts, `@warning` for dangerous misuse, and `@see` to point to related APIs. Add new public headers to the Doxygen input list or the public include directory and assign them to an existing group.

The native C ABI keeps its conventional C comments. A documentation-only filter makes those existing comments available to Doxygen, and the original Markdown reference generator still reads the unmodified ABI header.

The [coverage baseline](https://github.com/connorfrancey/mirage/blob/main/tools/documentation/coverage-baseline.json) records outstanding documentation gaps. The check lists undocumented public declarations by header and fails for gaps outside that baseline. Remove entries as declarations gain comments. Add documentation for new public declarations.

See the [Doxygen configuration manual](https://www.doxygen.nl/manual/config.html) and [MkDocs validation settings](https://www.mkdocs.org/user-guide/configuration/#validation) when maintaining the build.
