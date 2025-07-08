/* must match virglrenderer/src/apir-protocol.h */

#pragma once

#define VENUS_COMMAND_TYPE_LENGTH 331

typedef enum {
    APIR_COMMAND_TYPE_HandShake = 0,
    APIR_COMMAND_TYPE_LoadLibrary = 1,
    APIR_COMMAND_TYPE_Forward = 2,

    APIR_COMMAND_TYPE_LENGTH = 3,
} ApirCommandType;


typedef enum {
    APIR_LOAD_LIBRARY_SUCCESS = 0,
    APIR_LOAD_LIBRARY_HYPERCALL_ERROR = 1,
    APIR_LOAD_LIBRARY_ALREADY_LOADED = 2,
    APIR_LOAD_LIBRARY_ENV_VAR_MISSING = 3,
    APIR_LOAD_LIBRARY_CANNOT_OPEN = 4,
    APIR_LOAD_LIBRARY_SYMBOL_MISSING = 5,
    APIR_LOAD_LIBRARY_INIT_BASE_INDEX = 6, // anything above this is a APIR backend library initialization return code
} ApirLoadLibraryReturnCode;

typedef enum {
    APIR_FORWARD_SUCCESS = 0,
    APIR_FORWARD_NO_DISPATCH_FCT = 1,

    APIR_FORWARD_BASE_INDEX = 2, // anything above this is a APIR backend library forward return code
} ApirForwardReturnCode;

#define APIR_PROTOCOL_MAJOR 0
#define APIR_PROTOCOL_MINOR 1

#define APIR_HANDSHAKE_MAGIC 0xab1e

/* end of 'must match' */

static inline const char *api_remoting_command_name(int32_t type)
{
  switch (type) {
  case APIR_COMMAND_TYPE_HandShake: return "HandShake";
  case APIR_COMMAND_TYPE_LoadLibrary: return "LoadLibrary";
  case APIR_COMMAND_TYPE_Forward: return "Forward";
  default: return "unknown";
  }
}

static const char *apir_load_library_error(int code) {
#define APIR_LOAD_LIBRARY_ERROR(code_name) \
  do {						 \
    if (code == code_name) return #code_name;	 \
  } while (0)					 \

  APIR_LOAD_LIBRARY_ERROR(APIR_LOAD_LIBRARY_SUCCESS);
  APIR_LOAD_LIBRARY_ERROR(APIR_LOAD_LIBRARY_HYPERCALL_ERROR);
  APIR_LOAD_LIBRARY_ERROR(APIR_LOAD_LIBRARY_ALREADY_LOADED);
  APIR_LOAD_LIBRARY_ERROR(APIR_LOAD_LIBRARY_ENV_VAR_MISSING);
  APIR_LOAD_LIBRARY_ERROR(APIR_LOAD_LIBRARY_CANNOT_OPEN);
  APIR_LOAD_LIBRARY_ERROR(APIR_LOAD_LIBRARY_SYMBOL_MISSING);

  return "Unknown APIR_LoadLibrary error";

#undef APIR_LOAD_LIBRARY_ERROR
}
