// engine_node.c — N-API wrapper around the freestanding C engine (engine.c).
// Exports:
//   runCases(casesJsonString) -> Buffer(answersJson)   // Way A (string in/out)
//   runFiles(inPath, outPath) -> Buffer(answersJson)   // Way B (native file I/O)
// Both return the answers JSON text as a Buffer; the JS driver JSON.parses it.
// The engine itself never builds JS objects (that path is 10x slower than
// JSON.parse) and never touches libc: all I/O is direct syscalls (engine.c).
#include <node_api.h>
#include <stddef.h>
#include <stdint.h>

// engine.c public entry points (defined in engine.c, non-static).
typedef struct Arena Arena;
typedef struct Buf Buf;
int engine_run(const uint8_t *data, size_t len, Buf *out, Arena *arena);
int engine_run_file(const char *in_path, const char *out_path, Buf *out, Arena *arena);
// engine.c arena/buf helpers (non-static? they are static in engine.c — see below).
// NOTE: engine.c's arena_init/arena_reset/buf_init are static, so this file
// instead includes engine.c's private surface via a small public shim declared
// in engine.c (engine_alloc_arena / engine_free_arena / engine_new_buf).
Arena *engine_alloc_arena(size_t bytes);
void engine_free_arena(Arena *a);
Buf *engine_new_buf(Arena *a, size_t cap);
uint8_t *engine_buf_data(Buf *b);
size_t engine_buf_len(Buf *b);
uint8_t *engine_arena_alloc(Arena *a, size_t n);

#define CHECK(call) do { napi_status st = (call); if (st != napi_ok) { const napi_extended_error_info *info; napi_get_last_error_info(env, &info); napi_throw_error(env, NULL, info ? info->error_message : "napi error"); return NULL; } } while (0)

static napi_value run_cases(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1) { napi_throw_type_error(env, NULL, "runCases(casesJsonString)"); return NULL; }
  size_t len = 0;
  CHECK(napi_get_value_string_utf8(env, argv[0], NULL, 0, &len));
  // napi_get_value_string_utf8 reports the byte length (without NUL).
  uint8_t *data = NULL;
  // copy the JS string into arena-backed storage and run the engine on it.
  Arena *arena = engine_alloc_arena(256u << 20);
  if (!arena) { napi_throw_error(env, NULL, "arena alloc failed"); return NULL; }
  // copy input into arena-backed storage
  data = engine_arena_alloc(arena, len + 1);
  if (!data) { engine_free_arena(arena); napi_throw_error(env, NULL, "arena alloc failed"); return NULL; }
  size_t copied = 0;
  CHECK(napi_get_value_string_utf8(env, argv[0], (char *)data, len + 1, &copied));
  Buf *out = engine_new_buf(arena, 8u << 20);
  if (!out) { engine_free_arena(arena); napi_throw_error(env, NULL, "buf alloc failed"); return NULL; }
  int n = engine_run(data, len, out, arena);
  if (n < 0) { engine_free_arena(arena); napi_throw_error(env, NULL, "engine_run failed"); return NULL; }
  napi_value result = NULL;
  void *bufdata = NULL;
  CHECK(napi_create_buffer_copy(env, engine_buf_len(out), engine_buf_data(out), &bufdata, &result));
  engine_free_arena(arena);
  return result;
}

static napi_value run_files(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2) { napi_throw_type_error(env, NULL, "runFiles(inPath, outPath)"); return NULL; }
  char in_path[4096], out_path[4096];
  size_t in_len = 0, out_len = 0;
  CHECK(napi_get_value_string_utf8(env, argv[0], in_path, sizeof(in_path), &in_len));
  CHECK(napi_get_value_string_utf8(env, argv[1], out_path, sizeof(out_path), &out_len));
  Arena *arena = engine_alloc_arena(256u << 20);
  if (!arena) { napi_throw_error(env, NULL, "arena alloc failed"); return NULL; }
  Buf *out = engine_new_buf(arena, 8u << 20);
  if (!out) { engine_free_arena(arena); napi_throw_error(env, NULL, "buf alloc failed"); return NULL; }
  int n = engine_run_file(in_path, out_path, out, arena);
  if (n < 0) { engine_free_arena(arena); napi_throw_error(env, NULL, "engine_run_file failed"); return NULL; }
  napi_value result = NULL;
  void *bufdata = NULL;
  CHECK(napi_create_buffer_copy(env, engine_buf_len(out), engine_buf_data(out), &bufdata, &result));
  engine_free_arena(arena);
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn_cases = NULL, fn_files = NULL;
  CHECK(napi_create_function(env, "runCases", NAPI_AUTO_LENGTH, run_cases, NULL, &fn_cases));
  CHECK(napi_create_function(env, "runFiles", NAPI_AUTO_LENGTH, run_files, NULL, &fn_files));
  CHECK(napi_set_named_property(env, exports, "runCases", fn_cases));
  CHECK(napi_set_named_property(env, exports, "runFiles", fn_files));
  return exports;
}
