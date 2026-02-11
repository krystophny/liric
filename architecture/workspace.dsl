workspace "Liric Architecture" "Curated C4 model for liric with clickable links to source" {

    model {
        engineer = person "Compiler Engineer" "Runs liric directly, via LFortran integration, and through benchmark tooling."

        lfortran = softwareSystem "LFortran" "Fortran compiler that can route codegen through LLVM or liric compatibility layers."
        lli = softwareSystem "LLVM lli" "LLVM interpreter/JIT baseline used by liric benchmarks."
        host_runtime = softwareSystem "Host Runtime" "Host process symbols and shared libraries loaded via dlsym/dlopen for JIT symbol resolution."

        liric = softwareSystem "Liric" "Lightweight IR compiler and JIT/object emitter for LLVM IR and WebAssembly." {
            api_frontdoor = container "Public API + Frontend Registry" "API entrypoints for parsing/building/executing modules and frontend auto-detection." "C11" {
                tags "API"
                url "https://github.com/krystophny/liric/blob/main/include/liric/liric.h"
            }

            ll_frontend = container "LLVM IR Frontend" "Tokenizes and parses .ll text into liric SSA IR." "C11" {
                tags "Frontend"
                url "https://github.com/krystophny/liric/tree/main/src"

                ll_lexer = component "LL Lexer" "Token stream generation for LLVM IR text." "src/ll_lexer.c" {
                    tags "Frontend"
                    url "https://github.com/krystophny/liric/blob/main/src/ll_lexer.c"
                }

                ll_parser = component "LL Parser" "Recursive descent parser that constructs lr_module_t and instruction graph." "src/ll_parser.c" {
                    tags "Frontend"
                    url "https://github.com/krystophny/liric/blob/main/src/ll_parser.c"
                }

                frontend_registry_ll = component "Frontend Registry (LL path)" "Frontend selection and auto-detection routing for LL parsing." "src/frontend_registry.c" {
                    tags "API"
                    url "https://github.com/krystophny/liric/blob/main/src/frontend_registry.c"
                }
            }

            bc_frontend = container "LLVM Bitcode Frontend" "Parses LLVM .bc input directly into liric SSA IR (no .ll text conversion)." "C11" {
                tags "Frontend"
                url "https://github.com/krystophny/liric/tree/main/src"

                bc_decode_comp = component "BC Decoder" "Decodes LLVM bitcode and translates LLVM IR module/function/block/instruction graph into liric IR." "src/bc_decode.c" {
                    tags "Frontend"
                    url "https://github.com/krystophny/liric/blob/main/src/bc_decode.c"
                }

                frontend_registry_bc = component "Frontend Registry (BC path)" "Frontend selection and auto-detection routing for BC parsing." "src/frontend_registry.c" {
                    tags "API"
                    url "https://github.com/krystophny/liric/blob/main/src/frontend_registry.c"
                }
            }

            frontend_common = container "Frontend Common Helpers" "Shared symbol/function/error helpers reused across LL and BC parser frontends." "C11" {
                tags "Frontend"
                url "https://github.com/krystophny/liric/tree/main/src"

                frontend_common_comp = component "Frontend Common API" "Common parser helper functions for symbol interning, function creation, and diagnostics." "src/frontend_common.c" {
                    tags "Frontend"
                    url "https://github.com/krystophny/liric/blob/main/src/frontend_common.c"
                }
            }

            wasm_frontend = container "WASM Frontend" "Decodes .wasm binaries and converts stack machine semantics into SSA IR." "C11" {
                tags "Frontend"
                url "https://github.com/krystophny/liric/tree/main/src"

                wasm_decode = component "WASM Decoder" "Section decoder and LEB128 parser." "src/wasm_decode.c" {
                    tags "Frontend"
                    url "https://github.com/krystophny/liric/blob/main/src/wasm_decode.c"
                }

                wasm_to_ir = component "WASM-to-IR Converter" "Converts decoded WASM instructions into liric SSA module/function/block/inst graph." "src/wasm_to_ir.c" {
                    tags "Frontend"
                    url "https://github.com/krystophny/liric/blob/main/src/wasm_to_ir.c"
                }
            }

            core_ir = container "Core SSA IR + Builder" "Target-independent IR objects, types/opcodes, and arena-backed ownership model." "C11" {
                tags "Core"
                url "https://github.com/krystophny/liric/blob/main/src/ir.h"

                arena_alloc = component "Arena Allocator" "Chunked bump allocator owning IR lifetime." "src/arena.c" {
                    tags "Core"
                    url "https://github.com/krystophny/liric/blob/main/src/arena.c"
                }

                ir_model = component "IR Model" "lr_module_t/lr_func_t/lr_block_t/lr_inst_t graph and type/opcode definitions." "src/ir.c + src/ir.h" {
                    tags "Core"
                    url "https://github.com/krystophny/liric/blob/main/src/ir.c"
                }

                c_builder = component "C Builder API" "Programmatic construction of liric IR without parsing textual LL." "src/builder.c" {
                    tags "Core"
                    url "https://github.com/krystophny/liric/blob/main/src/builder.c"
                }
            }

            backends = container "Target Backends" "Host-target code generation (x86_64/aarch64) and shared target helpers." "C11" {
                tags "Backend"
                url "https://github.com/krystophny/liric/tree/main/src"

                target_registry = component "Target Registry" "Host detection and target lookup." "src/target_registry.c" {
                    tags "Backend"
                    url "https://github.com/krystophny/liric/blob/main/src/target_registry.c"
                }

                target_x86 = component "x86_64 Backend" "Instruction selection + encoding for x86_64 host JIT/object output." "src/target_x86_64.c" {
                    tags "Backend"
                    url "https://github.com/krystophny/liric/blob/main/src/target_x86_64.c"
                }

                target_arm = component "aarch64 Backend" "Instruction selection + encoding for arm64 host JIT/object output." "src/target_aarch64.c" {
                    tags "Backend"
                    url "https://github.com/krystophny/liric/blob/main/src/target_aarch64.c"
                }

                target_shared = component "Shared Target Helpers" "Cross-backend helpers and common lowering utilities." "src/target_shared.c + src/target_common.c" {
                    tags "Backend"
                    url "https://github.com/krystophny/liric/blob/main/src/target_shared.c"
                }
            }

            jit_runtime = container "JIT Runtime" "Executable memory, symbol resolution providers, and runtime module linking." "C11" {
                tags "Runtime"
                url "https://github.com/krystophny/liric/blob/main/src/jit.c"

                jit_exec = component "JIT Compiler/Linker" "Compiles functions, manages update transactions, resolves relocations." "src/jit.c" {
                    tags "Runtime"
                    url "https://github.com/krystophny/liric/blob/main/src/jit.c"
                }

                jit_symbols = component "Symbol Resolution" "JIT table lookup, loaded-library lookup, and process symbol fallback." "src/jit.c" {
                    tags "Runtime"
                    url "https://github.com/krystophny/liric/blob/main/src/jit.c"
                }

                jit_memory = component "W^X Memory Manager" "mmap/mprotect/MAP_JIT handling for executable code pages." "src/jit.c" {
                    tags "Runtime"
                    url "https://github.com/krystophny/liric/blob/main/src/jit.c"
                }
            }

            obj_emit = container "Object + Executable Emission" "Emission path for relocatable objects and standalone executables from liric IR." "C11" {
                tags "ObjEmit"
                url "https://github.com/krystophny/liric/blob/main/src/objfile.c"

                obj_orchestrator = component "Objfile Orchestrator" "Coordinates backend compile output and object/executable writer sections/relocations." "src/objfile.c" {
                    tags "ObjEmit"
                    url "https://github.com/krystophny/liric/blob/main/src/objfile.c"
                }

                obj_elf = component "ELF Writer" "Linux ELF relocatable and standalone executable emission." "src/objfile_elf.c" {
                    tags "ObjEmit"
                    url "https://github.com/krystophny/liric/blob/main/src/objfile_elf.c"
                }

                obj_macho = component "Mach-O Writer" "macOS Mach-O object emission." "src/objfile_macho.c" {
                    tags "ObjEmit"
                    url "https://github.com/krystophny/liric/blob/main/src/objfile_macho.c"
                }
            }

            compat_layer = container "Compatibility Layer" "LLVM-style builder compatibility in C and header-only C++ wrappers." "C11/C++17 headers" {
                tags "Compat"
                url "https://github.com/krystophny/liric/tree/main/include/llvm"

                compat_c = component "C Compat API" "lc_* handle-based builder API and deferred PHI handling." "src/liric_compat.c" {
                    tags "Compat"
                    url "https://github.com/krystophny/liric/blob/main/src/liric_compat.c"
                }

                compat_cpp = component "LLVM C++ Header Shim" "Header-only LLVM API facade mapping to liric compat primitives." "include/llvm/**/*.h" {
                    tags "Compat"
                    url "https://github.com/krystophny/liric/tree/main/include/llvm"
                }

                compat_types = component "Public Compat Types" "Shared type definitions consumed by C/C++ compatibility layers." "include/liric/liric_types.h" {
                    tags "Compat"
                    url "https://github.com/krystophny/liric/blob/main/include/liric/liric_types.h"
                }
            }

            tools = container "CLI, Tests, and Benchmarks" "Command-line tool, probe runner, and performance/correctness benchmarks." "C11" {
                tags "Tools"
                url "https://github.com/krystophny/liric/tree/main/tools"
            }

            !docs docs
            !adrs adrs
        }

        engineer -> liric "Reads architecture and runs build/test/bench/JIT workflows" "CLI and scripts"
        engineer -> lfortran "Builds and runs LFortran integration benchmarks" "CLI and scripts"

        lfortran -> compat_layer "Generates/consumes LLVM-style APIs through liric compatibility path" "LLVM-compatible C/C++ API"

        api_frontdoor -> ll_frontend "Routes .ll parsing" "frontend dispatch"
        api_frontdoor -> bc_frontend "Routes .bc parsing" "frontend dispatch"
        api_frontdoor -> wasm_frontend "Routes .wasm parsing" "frontend dispatch"
        api_frontdoor -> core_ir "Owns module lifecycle and builder entrypoints" "C API calls"
        api_frontdoor -> jit_runtime "Creates JIT, adds modules, resolves functions" "C API calls"
        api_frontdoor -> obj_emit "Emits object files and standalone executables from liric modules" "C API calls"

        ll_frontend -> core_ir "Builds lr_module_t from text IR" "IR builder calls"
        bc_frontend -> core_ir "Builds lr_module_t from LLVM bitcode decode path" "IR builder calls"
        wasm_frontend -> core_ir "Builds lr_module_t from binary WASM" "IR builder calls"
        ll_frontend -> frontend_common "Reuses shared frontend helpers" "common parser helpers"
        bc_frontend -> frontend_common "Reuses shared frontend helpers" "common parser helpers"
        frontend_common -> core_ir "Creates symbols/functions shared by parser frontends" "IR API helpers"

        compat_layer -> core_ir "Builds/updates IR via lc_* builders" "compat builder API"
        compat_layer -> api_frontdoor "Uses public module/JIT interfaces" "public C API"

        core_ir -> backends "Supplies target-independent SSA for codegen" "compile interface"
        backends -> jit_runtime "Produces machine code for in-memory execution" "code buffer"
        backends -> obj_emit "Produces machine code/relocations for object/executable output" "code sections + relocations"

        obj_emit -> host_runtime "Objects are linked by host linker/runtime; standalone executables run directly" "ELF/Mach-O and runtime"

        jit_runtime -> host_runtime "Loads shared libraries and resolves symbols" "dlopen/dlsym"
        jit_runtime -> backends "Invokes host-compatible target compiler" "compile interface"

        tools -> api_frontdoor "CLI parse/dump/jit/object/executable workflows" "direct API usage"
        tools -> jit_runtime "Probe runner and JIT execution harness" "JIT invocation"
        tools -> lli "Benchmarks compare liric against lli baseline" "subprocess benchmark harness"

        frontend_registry_ll -> ll_lexer "Dispatches LL parse path" "function call"
        frontend_registry_bc -> bc_decode_comp "Dispatches BC parse path" "function call"
        ll_lexer -> ll_parser "Token stream" "in-memory token API"
        ll_parser -> ir_model "Builds module/function/block/instruction graph" "IR construction"
        ll_parser -> arena_alloc "Allocates parser-owned nodes" "arena allocation"

        wasm_decode -> wasm_to_ir "Decoded sections and instructions" "in-memory model"
        wasm_to_ir -> ir_model "Constructs SSA IR" "IR construction"
        wasm_to_ir -> arena_alloc "Allocates converted IR nodes" "arena allocation"

        c_builder -> ir_model "Appends IR instructions/types/blocks" "builder API"
        c_builder -> arena_alloc "Allocates builder-created objects" "arena allocation"
        ir_model -> arena_alloc "Owns all IR object lifetime" "arena ownership"

        target_registry -> target_x86 "Resolves x86_64 backend" "target lookup"
        target_registry -> target_arm "Resolves aarch64 backend" "target lookup"
        target_shared -> target_x86 "Shared lowering/encoding helpers" "internal helpers"
        target_shared -> target_arm "Shared lowering/encoding helpers" "internal helpers"
        ir_model -> target_shared "Supplies IR to backend lowering" "compile interface"

        jit_exec -> target_registry "Selects host backend" "target lookup"
        jit_exec -> target_shared "Compiles IR to machine code" "compile interface"
        jit_exec -> jit_memory "Writes executable pages with W^X discipline" "mmap/mprotect"
        jit_exec -> jit_symbols "Resolves external and JIT symbols" "symbol lookup"
        jit_symbols -> host_runtime "dlsym/dlopen fallback resolution" "dynamic linker"

        obj_orchestrator -> target_registry "Selects host backend" "target lookup"
        obj_orchestrator -> target_shared "Compiles IR for object emission" "compile interface"
        obj_orchestrator -> obj_elf "Writes ELF relocatable/executable output" "ELF writer API"
        obj_orchestrator -> obj_macho "Writes Mach-O object output" "Mach-O writer API"

        compat_cpp -> compat_c "Maps LLVM C++ APIs to lc_* calls" "header wrappers"
        compat_c -> compat_types "Uses shared handle/type definitions" "C headers"
        compat_c -> c_builder "Builds liric IR programmatically" "builder API"
        compat_c -> api_frontdoor "Uses module/JIT lifecycle APIs" "public C API"
    }

    views {
        systemContext liric "SystemContext" {
            include *
            autolayout lr
            title "Liric System Context"
            description "Top-level integration boundaries around liric in development and benchmarking workflows."
        }

        container liric "Containers" {
            include *
            autolayout lr
            title "Liric Container View"
            description "Primary subsystems and major internal flow: parse/build -> IR -> backend -> JIT/object/executable output."
        }

        component ll_frontend "LLFrontendComponents" {
            include *
            autolayout lr
            title "LLVM IR Frontend Components"
        }

        component bc_frontend "BCFrontendComponents" {
            include *
            autolayout lr
            title "LLVM Bitcode Frontend Components"
        }

        component frontend_common "FrontendCommonComponents" {
            include *
            autolayout lr
            title "Frontend Common Helper Components"
        }

        component wasm_frontend "WasmFrontendComponents" {
            include *
            autolayout lr
            title "WASM Frontend Components"
        }

        component core_ir "CoreIRComponents" {
            include *
            autolayout lr
            title "Core IR Components"
        }

        component backends "BackendComponents" {
            include *
            autolayout lr
            title "Backend Components"
        }

        component jit_runtime "JITComponents" {
            include *
            autolayout lr
            title "JIT Runtime Components"
        }

        component obj_emit "ObjectEmissionComponents" {
            include *
            autolayout lr
            title "Object Emission Components"
        }

        component compat_layer "CompatComponents" {
            include *
            autolayout lr
            title "Compatibility Layer Components"
        }

        styles {
            element "Person" {
                background "#f4f4f5"
                color "#111827"
                shape person
            }
            element "Software System" {
                background "#0f172a"
                color "#f8fafc"
            }
            element "Container" {
                background "#1f2937"
                color "#f9fafb"
            }
            element "Component" {
                background "#334155"
                color "#f8fafc"
            }
            element "API" {
                background "#0b3c5d"
            }
            element "Frontend" {
                background "#14532d"
            }
            element "Core" {
                background "#78350f"
            }
            element "Backend" {
                background "#3f3f46"
            }
            element "Runtime" {
                background "#7f1d1d"
            }
            element "ObjEmit" {
                background "#4c1d95"
            }
            element "Compat" {
                background "#1e3a8a"
            }
            element "Tools" {
                background "#065f46"
            }
        }

        theme default
    }

    configuration {
        scope softwaresystem
    }
}
