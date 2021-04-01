#include <stdint.h>
#include <assert.h>

#include <mprompt.h>
#include "mphnd.h"

#ifdef __cplusplus
#define MP_HAS_TRY  (1)
#include <exception>
#else
#define MP_HAS_TRY  (0)
#endif


#if defined(_MSC_VER)
#define mph_decl_noinline        __declspec(noinline)
#define mph_decl_thread          __declspec(thread)
#elif (defined(__GNUC__) && (__GNUC__>=3))  // includes clang and icc
#define mph_decl_noinline        __attribute__((noinline))
#define mph_decl_thread          __thread
#else
#define mph_decl_noinline
#define mph_decl_thread          __thread  
#endif

#if defined(__GNUC__) || defined(__clang__)
#define mph_unlikely(x)          __builtin_expect((x),0)
#define mph_likely(x)            __builtin_expect((x),1)
#else
#define mph_unlikely(x)          (x)
#define mph_likely(x)            (x)
#endif

#define mph_assert(x)            assert(x)
#define mph_assert_internal(x)   mph_assert(x)


//---------------------------------------------------------------------------
// Builtin handler kinds
//---------------------------------------------------------------------------

mph_kind_t MPH_FINALLY = "mph_finally";
mph_kind_t MPH_UNDER   = "mph_under";
mph_kind_t MPH_MASK    = "mph_mask";


//---------------------------------------------------------------------------
// Internal handlers
//---------------------------------------------------------------------------

// Internal base handler
typedef struct mph_handler_s {
  struct mph_handler_s*  parent;
  mp_prompt_t*           prompt;   // NULL for linear handlers
  mph_kind_t             kind;
  void*                  hdata;
} mph_handler_t;


// An under handler (used for tail-resumptive optimization)
typedef struct mph_handler_under_s {
  mph_handler_t  handler;
  mph_kind_t     under;     // ignore handlers until the innermost `under` kind
} mph_handler_under_t;


// A mask handler
typedef struct mph_handler_mask_s {
  mph_handler_t  handler;
  mph_kind_t     mask;    
  size_t         from;
} mph_handler_mask_t;


static bool mph_is_prompt_handler(mph_handler_t* h) {
  return (h->prompt != NULL);
}

//---------------------------------------------------------------------------
// Shadow stack
//---------------------------------------------------------------------------

// Top of the handlers in the current execution stack
mph_decl_thread mph_handler_t* _mph_top;

mph_handler_t* mph_top(void) {
  return _mph_top;  
}

mph_handler_t* mph_parent(mph_handler_t* handler) {
  return (mph_unlikely(handler == NULL) ? mph_top() : handler->parent);
}

mph_kind_t mph_kind(mph_handler_t* handler) {
  return handler->kind;
}
void* mph_data(mph_handler_t* handler) {
  return handler->hdata;
}



//---------------------------------------------------------------------------
// Find innermost handler
//---------------------------------------------------------------------------

// Find the innermost handler
mph_handler_t* mph_find(mph_kind_t kind) {
  mph_handler_t* h = _mph_top;
  size_t mask_level = 0;
  while (mph_likely(h != NULL)) {
    mph_kind_t hkind = h->kind;
    if (mph_likely(kind == hkind)) {
      if (mph_likely(mask_level <= 0)) {
        return h;
      }
      else {
        mask_level--;
      }
    }
    else if (mph_unlikely(hkind == MPH_UNDER)) {
      // skip to the matching handler of this `under` frame
      mph_kind_t ukind = ((mph_handler_under_t*)h)->under;
      do {
        h = h->parent;
      } while (h!= NULL && h->kind != ukind);
      if (h == NULL) break;
    }
    else if (mph_unlikely(hkind == MPH_MASK)) {
      // increase masking level 
      mph_handler_mask_t* m = ((mph_handler_mask_t*)h);
      if (m->mask == kind && m->from <= mask_level) {
        mask_level++;
      }
    }
    h = h->parent;
  }
  return NULL;
}



//---------------------------------------------------------------------------
// Linear handlers without a prompt
//---------------------------------------------------------------------------


// use as: `{mpe_with_handler(f){ <body> }}`
#if MP_HAS_TRY
#define mph_with_handler(f)   mph_raii_with_handler_t _with_handler(f); 
// This class ensures a handler-stack will be properly unwound even when exceptions are raised.
class mph_raii_with_handler_t {
private:
  mph_handler_t* f;
public:
  mph_raii_with_handler_t(mph_handler_t* f) {
    this->f = f;
    f->parent = _mph_top;
    _mph_top = f;
  }
  ~mph_raii_with_handler_t() {
    mph_assert_internal(_mph_top == f);
    _mph_top = f->parent;
  }
};
#else
// C version
#define mph_with_handler(f) \
  for( bool _once = ((f)->parent = _mph_top, _mph_top = (f), true); \
       _once; \
       _once = (_mph_top = (f)->parent, false) ) 
#endif

void* mph_linear_handler(mph_kind_t kind, void* hdata, mph_start_fun_t* fun, void* arg) {
  mph_handler_t h;
  h.kind = kind;
  h.hdata = hdata;
  h.prompt = NULL;
  void* result = NULL;
  {mph_with_handler(&h){
    result = fun(h.hdata,arg);
  }}
  return result;
}



//---------------------------------------------------------------------------
// Abort
//---------------------------------------------------------------------------

static void* mph_abort_fun(mp_resume_t* r, void* arg) {
  mp_resume_drop(r);
  return arg;
}

// Yield to a prompt without unwinding
static void* mph_abort_to(mph_handler_t* h, void* arg) {
  mph_assert(mph_is_prompt_handler(h));
  return mp_yield(h->prompt, &mph_abort_fun, arg);
}



//---------------------------------------------------------------------------
// Unwind
//---------------------------------------------------------------------------

#if MP_HAS_TRY
class mph_unwind_exception : public std::exception {
public:
  mph_handler_t* target;
  mph_unwind_fun_t* fun;
  void* arg;
  mph_unwind_exception(mph_handler_t* h, mph_unwind_fun_t* fun, void* arg) : target(h), fun(fun), arg(arg) {  }
  mph_unwind_exception(const mph_unwind_exception& e) : target(e.target), fun(e.fun), arg(e.arg) { }
  mph_unwind_exception& operator=(const mph_unwind_exception& e) {
    target = e.target; fun = e.fun; arg = e.arg;
    return *this;
  }

  virtual const char* what() const throw() {
    return "libmpeff: unwinding the stack; do not catch this exception!";
  }
};

static void mph_unwind_to(mph_handler_t* target, mph_unwind_fun_t* fun, void* arg) {
  throw mph_unwind_exception(target, fun, arg);
}
#else
static void mph_unwind_to(mph_handler_t* target, mph_unwind_fun_t* fun, void* arg) {
  // TODO: walk the handlers and invoke finally frames
  // invoke the unwind function under an "under" frame
  // and finally yield up to abort
  mph_abort_to(target, arg);
}
#endif



//---------------------------------------------------------------------------
// Full prompt handler: can be yielded to (or unwound to)
//---------------------------------------------------------------------------


// Pass arguments down in a closure environment
struct mph_start_env {
  mph_kind_t       kind;
  size_t           hdata_size;
  mph_start_fun_t* body;
  void*            arg;
};

// Start a handler
static void* mph_start(mp_prompt_t* prompt, void* earg) {
  // init
  struct mph_start_env* env = (struct mph_start_env*)earg;  
  void* hdata = alloca(env->hdata_size);
  mph_handler_t h;
  h.kind = env->kind;
  h.hdata = hdata;
  h.prompt = prompt;
#if MP_HAS_TRY
  try {  // catch unwind exceptions
#endif
    void* result = NULL;
    {mph_with_handler(&h) {
      result = (env->body)(hdata,env->arg);
    }}
    return result;
#if MP_HAS_TRY
  }     // handle unwind exceptions
  catch (const mph_unwind_exception& e) {
    if (e.target != &h) {
      throw;  // rethrow 
    }
    return e.fun(hdata, e.arg);  // execute the unwind function here while hdata is valid 
  }
#endif  
}


void* mph_prompt(mph_kind_t kind, size_t hdata_size, mph_start_fun_t* fun, void* arg) {
  struct mph_start_env env = { kind, hdata_size, fun, arg };
  return mp_prompt(&mph_start, &env);
}


//---------------------------------------------------------------------------
// Yield
//---------------------------------------------------------------------------

typedef struct mph_yield_env_s {
  void*            hdata;
  mph_yield_fun_t* fun;
  void*            arg;
} mph_yield_env_t;

typedef struct mph_resume_env_s {
  void* result;
  bool  unwind;
} mph_resume_env_t;


static void* mph_yield_fun(mp_resume_t* r, void* envarg) {
  mph_yield_env_t* env = (mph_yield_env_t*)envarg;
  return (env->fun)((mph_resume_t*)r, env->hdata, env->arg);
}


static void* mph_unwind_fun(void* hdata, void* arg) {
  (void)(hdata);
  return arg;
}

// Yield to a prompt without unwinding
static void* mph_yield_to_internal(bool once, mph_handler_t* h, mph_yield_fun_t fun, void* arg) {
  mph_assert(mph_is_prompt_handler(h));
  
  // unlink the current handler top
  mph_handler_t* yield_top = _mph_top; 
  _mph_top = h->parent;                

  // yield
  mph_yield_env_t yenv = { &h->hdata, fun, arg };
  mph_resume_env_t* renv = (mph_resume_env_t*)
                           (mph_likely(once) ? mp_yield(h->prompt, &mph_yield_fun, &yenv) 
                                             : mp_myield(h->prompt, &mph_yield_fun, &yenv));
 
  // and relink handlers once resumed
  _mph_top = yield_top;
  h->parent = _mph_top;                

  // unwind or return?
  if (mph_unlikely(renv->unwind)) {
    mph_unwind_to(h, &mph_unwind_fun, renv->result);
    return NULL;
  }
  else {
    return renv->result;
  }
}

// Yield to a prompt without unwinding
void* mph_yield_to(mph_handler_t* h, mph_yield_fun_t fun, void* arg) {
  return mph_yield_to_internal(true, h, fun, arg);
}

// Multi-shot Yield to a prompt without unwinding
void* mph_myield_to(mph_handler_t* h, mph_yield_fun_t fun, void* arg) {
  return mph_yield_to_internal(false, h, fun, arg);
}


//---------------------------------------------------------------------------
// Resuming
//---------------------------------------------------------------------------

// mph_resume_t* is always cast to mp_resume_t*
struct mp_resume_s {
  void* _abstract;
};

void* mph_resume(mph_resume_t* resume, void* arg) {
  mph_resume_env_t renv = { arg, false };
  return mp_resume((mp_resume_t*)resume, &renv);
}

void* mph_resume_tail(mph_resume_t* resume, void* arg) {
  mph_resume_env_t renv = { arg, false };
  return mp_resume_tail((mp_resume_t*)resume, &renv);
}

void mph_resume_unwind(mph_resume_t* resume) {
  mph_resume_env_t renv = { NULL, true /* unwind */ };
  mp_resume((mp_resume_t*)resume, &renv);
}


void mph_resume_drop(mph_resume_t* resume) {
  mp_resume_t* r = (mp_resume_t*)resume;
  if (mp_resume_should_unwind(r)) {
    mph_resume_unwind(resume);
  }
  else {
    mp_resume_drop(r);
  }
}


//---------------------------------------------------------------------------
// Under
//---------------------------------------------------------------------------

void* mph_under(mph_kind_t under, void* (*fun)(void*), void* arg) {
  mph_handler_under_t h;
  h.under = under;
  h.handler.kind = MPH_UNDER;
  h.handler.hdata = NULL;
  void* result = NULL;
  {mph_with_handler(&h.handler){
    result = fun(arg);
  }}
  return result;
}


//---------------------------------------------------------------------------
// Mask
//---------------------------------------------------------------------------

void* mph_mask(mph_kind_t mask, size_t from, void* (*fun)(void*), void* arg) {
  mph_handler_mask_t h;
  h.mask = mask;
  h.from = from;
  h.handler.kind = MPH_UNDER;
  h.handler.hdata = NULL;
  void* result = NULL;
  {mph_with_handler(&h.handler) {
    result = fun(arg);
  }}
  return result;
}
