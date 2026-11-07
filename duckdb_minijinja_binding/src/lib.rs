use std::borrow::Cow;
use std::collections::HashMap;
use std::error::Error as _;
use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;
use std::sync::Arc;

use minijinja::value::ValueKind;
use minijinja::{AutoEscape, Environment, UndefinedBehavior, Value};

/// The name MiniJinja gives a template rendered from a string; inline templates are registered under the
/// same name so that error messages and `autoescape_extensions` matching stay as they were.
const INLINE_TEMPLATE_NAME: &str = "<string>";

/// The most operators a single `{{ ... }}` / `{% ... %}` tag may contain.
///
/// MiniJinja's parser limits nesting through brackets, but operator chains such as `1 + 1 + ... + 1`,
/// `a.b.c...` or `x|f|f|...` are parsed in loops into equally deep trees that later passes walk
/// recursively, so a long enough chain overflows the stack and aborts the process. No sensible template
/// comes close to this limit.
#[cfg(not(target_family = "wasm"))]
const MAX_TAG_OPERATORS: usize = 10_000;
#[cfg(target_family = "wasm")]
const MAX_TAG_OPERATORS: usize = 1_000;

/// Parsing and rendering run with at least this much stack available, on a temporary stack if the
/// current thread has less left (DuckDB worker threads get 512 KiB on macOS). It covers the deepest
/// expression MAX_TAG_OPERATORS allows with a wide margin.
#[cfg(not(target_family = "wasm"))]
const STACK_RED_ZONE: usize = 4 * 1024 * 1024;
#[cfg(not(target_family = "wasm"))]
const STACK_SIZE: usize = 16 * 1024 * 1024;

#[cfg(not(target_family = "wasm"))]
fn with_stack<R>(f: impl FnOnce() -> R) -> R {
    stacker::maybe_grow(STACK_RED_ZONE, STACK_SIZE, f)
}

#[cfg(target_family = "wasm")]
fn with_stack<R>(f: impl FnOnce() -> R) -> R {
    f()
}

/// Rejects templates with a tag that chains more than MAX_TAG_OPERATORS operators (see there).
///
/// This is a conservative lexical scan: it skips comments and string literals and counts operator
/// characters and operator keywords inside each tag.
fn check_template_complexity(name: &str, source: &str) -> Result<(), String> {
    let bytes = source.as_bytes();
    let mut i = 0;
    while i + 1 < bytes.len() {
        if bytes[i] != b'{' {
            i += 1;
            continue;
        }
        let close: &[u8] = match bytes[i + 1] {
            b'{' => b"}}",
            b'%' => b"%}",
            b'#' => {
                // A comment: skip to its end
                i += 2;
                while i + 1 < bytes.len() && !(bytes[i] == b'#' && bytes[i + 1] == b'}') {
                    i += 1;
                }
                i += 2;
                continue;
            }
            _ => {
                i += 1;
                continue;
            }
        };
        i += 2;
        let mut operators = 0usize;
        while i < bytes.len() {
            let b = bytes[i];
            if b == close[0] && i + 1 < bytes.len() && bytes[i + 1] == close[1] {
                i += 2;
                break;
            }
            match b {
                b'"' | b'\'' => {
                    // A string literal: skip it, honouring backslash escapes
                    i += 1;
                    while i < bytes.len() && bytes[i] != b {
                        if bytes[i] == b'\\' {
                            i += 1;
                        }
                        i += 1;
                    }
                    i += 1;
                    continue;
                }
                b'+' | b'-' | b'*' | b'/' | b'%' | b'~' | b'|' | b'.' | b'<' | b'>' | b'='
                | b'!' | b'[' | b'(' => operators += 1,
                b'a'..=b'z' | b'A'..=b'Z' | b'_' => {
                    let start = i;
                    while i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_')
                    {
                        i += 1;
                    }
                    if matches!(
                        &bytes[start..i],
                        b"and" | b"or" | b"not" | b"is" | b"in" | b"if"
                    ) {
                        operators += 1;
                    }
                    if operators > MAX_TAG_OPERATORS {
                        break;
                    }
                    continue;
                }
                _ => {}
            }
            if operators > MAX_TAG_OPERATORS {
                return Err(format!(
                    "MiniJinja render error: template {:?} is too complex: a tag contains more than {} operators",
                    name, MAX_TAG_OPERATORS
                ));
            }
            i += 1;
        }
        if operators > MAX_TAG_OPERATORS {
            return Err(format!(
                "MiniJinja render error: template {:?} is too complex: a tag contains more than {} operators",
                name, MAX_TAG_OPERATORS
            ));
        }
    }
    Ok(())
}

#[repr(C)]
pub enum ResultCString {
    Ok(*mut c_char),
    Err(*mut c_char),
}

/// A template loaded by the caller (for `template_path`): its name and its source text.
#[repr(C)]
pub struct TemplateSource {
    pub name: *const c_char,
    pub name_len: usize,
    pub source: *const c_char,
    pub source_len: usize,
}

/// How undefined variables are treated. The caller validates the user's choice; unknown codes are errors.
pub const UNDEFINED_LENIENT: u8 = 0;
pub const UNDEFINED_STRICT: u8 = 1;
pub const UNDEFINED_SEMI_STRICT: u8 = 2;
pub const UNDEFINED_CHAINABLE: u8 = 3;

/// The options an environment is built with.
#[repr(C)]
pub struct EnvironmentOptions {
    pub autoescape: bool,
    pub undefined_behavior: u8,
    /// File name suffixes that switch on HTML escaping (when non-empty, only matching templates are escaped)
    pub autoescape_on: *const *const c_char,
    pub autoescape_on_count: usize,
    /// The maximum number of instructions a single render may execute; 0 means unlimited
    pub fuel: u64,
}

/// A configured environment holding parsed templates; it can render any number of times.
///
/// For an inline template it holds that single template; for `template_path` it holds a loader over
/// the templates the caller read, and the template to render is chosen by name for each call.
pub struct CompiledEnvironment {
    env: Environment<'static>,
    inline: bool,
}

#[repr(C)]
pub enum CompileResult {
    Ok(*mut CompiledEnvironment),
    Err(*mut c_char),
}

/// Borrows `len` bytes at `ptr` as a `&str`. DuckDB only passes valid UTF-8 VARCHAR data; invalid UTF-8
/// is replaced rather than trusted.
unsafe fn borrow_str<'a>(ptr: *const c_char, len: usize) -> Cow<'a, str> {
    if ptr.is_null() || len == 0 {
        return Cow::Borrowed("");
    }
    // `c_char` is `i8` on some platforms, so the cast is not always a no-op
    #[allow(clippy::unnecessary_cast)]
    let bytes = slice::from_raw_parts(ptr as *const u8, len);
    String::from_utf8_lossy(bytes)
}

fn into_c_string(s: String) -> *mut c_char {
    // A C string cannot hold NUL bytes. Rendered output and error messages may contain them (they can
    // come from JSON "\u0000" escapes), so they are dropped rather than panicking across the FFI.
    let bytes: Vec<u8> = s.into_bytes().into_iter().filter(|&b| b != 0).collect();
    // SAFETY: every NUL byte was removed above.
    unsafe { CString::from_vec_unchecked(bytes) }.into_raw()
}

/// Describes a caught panic so it can be reported as an ordinary error instead of unwinding into C++.
fn panic_message(payload: Box<dyn std::any::Any + Send>) -> String {
    let detail = if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    };
    format!("MiniJinja internal error: {}", detail)
}

/// Formats a MiniJinja error together with its chain of causes.
fn format_error(err: &minijinja::Error) -> String {
    let mut msg = format!("MiniJinja render error: {:?}\n", err);
    let mut source = err.source();
    while let Some(s) = source {
        msg.push_str(&format!("Caused by: {}\n", s));
        source = s.source();
    }
    msg
}

fn build_environment(
    inline_source: Option<String>,
    templates: Option<HashMap<String, String>>,
    options: &EnvironmentOptions,
    autoescape_on: Vec<String>,
) -> Result<CompiledEnvironment, String> {
    let mut env = Environment::new();

    env.set_undefined_behavior(match options.undefined_behavior {
        UNDEFINED_LENIENT => UndefinedBehavior::Lenient,
        UNDEFINED_STRICT => UndefinedBehavior::Strict,
        UNDEFINED_SEMI_STRICT => UndefinedBehavior::SemiStrict,
        UNDEFINED_CHAINABLE => UndefinedBehavior::Chainable,
        other => return Err(format!("invalid undefined_behavior code {}", other)),
    });

    if options.fuel > 0 {
        env.set_fuel(Some(options.fuel));
    }

    // Deterministic extras from minijinja-contrib: Python string/dict/list methods (`'x'.upper()`,
    // `d.items()`, ...) and the pluralize, filesizeformat, truncate, striptags, wordcount and wordwrap
    // filters plus the cycler and joiner functions. The contrib features that read the clock or a random
    // source are not enabled, so rendering stays deterministic.
    env.set_unknown_method_callback(minijinja_contrib::pycompat::unknown_method_callback);
    minijinja_contrib::add_to_environment(&mut env);

    if !options.autoescape && autoescape_on.is_empty() {
        env.set_auto_escape_callback(|_| AutoEscape::None);
    } else if !autoescape_on.is_empty() {
        env.set_auto_escape_callback(move |name| {
            if autoescape_on.iter().any(|ext| name.ends_with(ext.as_str())) {
                AutoEscape::Html
            } else {
                AutoEscape::None
            }
        });
    } else {
        env.set_auto_escape_callback(|_| AutoEscape::Html);
    }

    if let Some(templates) = templates {
        let templates = Arc::new(templates);
        env.set_loader(move |name| match templates.get(name) {
            Some(source) => {
                check_template_complexity(name, source).map_err(|msg| {
                    minijinja::Error::new(minijinja::ErrorKind::InvalidOperation, msg)
                })?;
                Ok(Some(source.clone()))
            }
            None => Ok(None),
        });
    }

    let inline = inline_source.is_some();
    if let Some(source) = inline_source {
        check_template_complexity(INLINE_TEMPLATE_NAME, &source)?;
        env.add_template_owned(INLINE_TEMPLATE_NAME, source)
            .map_err(|e| format_error(&e))?;
    }
    Ok(CompiledEnvironment { env, inline })
}

/// Builds an environment for rendering.
///
/// Without `use_templates` the environment holds the single inline template `source` (parsed now, so
/// syntax errors are reported here). With it, the environment serves the given templates (possibly none)
/// by name and `source` is ignored. Free the result with `free_compiled_environment` / `free_compile_error`.
///
/// # Safety
/// The pointers must be valid for the given lengths for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn compile_environment(
    source: *const c_char,
    source_len: usize,
    use_templates: bool,
    templates: *const TemplateSource,
    templates_count: usize,
    options: *const EnvironmentOptions,
) -> CompileResult {
    let result = catch_unwind(AssertUnwindSafe(|| {
        with_stack(|| {
            if options.is_null() {
                return Err("missing environment options".to_string());
            }
            let options = &*options;
            let mut autoescape_on = Vec::new();
            if options.autoescape_on_count > 0 && !options.autoescape_on.is_null() {
                for &ptr in
                    slice::from_raw_parts(options.autoescape_on, options.autoescape_on_count)
                {
                    if !ptr.is_null() {
                        autoescape_on.push(CStr::from_ptr(ptr).to_string_lossy().into_owned());
                    }
                }
            }
            if use_templates {
                let mut map = HashMap::with_capacity(templates_count);
                if templates_count > 0 && !templates.is_null() {
                    for t in slice::from_raw_parts(templates, templates_count) {
                        map.insert(
                            borrow_str(t.name, t.name_len).into_owned(),
                            borrow_str(t.source, t.source_len).into_owned(),
                        );
                    }
                }
                build_environment(None, Some(map), options, autoescape_on)
            } else {
                build_environment(
                    Some(borrow_str(source, source_len).into_owned()),
                    None,
                    options,
                    autoescape_on,
                )
            }
        })
    }));
    match result {
        Ok(Ok(env)) => CompileResult::Ok(Box::into_raw(Box::new(env))),
        Ok(Err(msg)) => CompileResult::Err(into_c_string(msg)),
        Err(payload) => CompileResult::Err(into_c_string(panic_message(payload))),
    }
}

fn parse_context(json: &str) -> Result<Value, String> {
    let ctx: Value = serde_json::from_str(json).map_err(|e| format!("Invalid JSON: {}", e))?;
    if ctx.kind() != ValueKind::Map {
        return Err(format!(
            "Invalid JSON: the template context must be a JSON object, not {}",
            ctx.kind()
        ));
    }
    Ok(ctx)
}

/// Renders a template of a compiled environment with a JSON object as context. For an inline
/// environment `name` is ignored; otherwise it is the name of the template to render.
///
/// # Safety
/// `compiled` must come from `compile_environment` and not have been freed; the other pointers must be
/// valid for the given lengths for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn render_compiled(
    compiled: *const CompiledEnvironment,
    name: *const c_char,
    name_len: usize,
    json_context: *const c_char,
    json_context_len: usize,
) -> ResultCString {
    let result = catch_unwind(AssertUnwindSafe(|| {
        with_stack(|| {
            if compiled.is_null() {
                return Err("missing compiled environment".to_string());
            }
            let compiled = &*compiled;
            let ctx = parse_context(&borrow_str(json_context, json_context_len))?;
            let name = if compiled.inline {
                Cow::Borrowed(INLINE_TEMPLATE_NAME)
            } else {
                borrow_str(name, name_len)
            };
            compiled
                .env
                .get_template(&name)
                .and_then(|tmpl| tmpl.render(&ctx))
                .map_err(|e| format_error(&e))
        })
    }));
    match result {
        Ok(Ok(output)) => ResultCString::Ok(into_c_string(output)),
        Ok(Err(msg)) => ResultCString::Err(into_c_string(msg)),
        Err(payload) => ResultCString::Err(into_c_string(panic_message(payload))),
    }
}

/// # Safety
/// `compiled` must come from `compile_environment` and be freed only once.
#[no_mangle]
pub unsafe extern "C" fn free_compiled_environment(compiled: *mut CompiledEnvironment) {
    if !compiled.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            with_stack(|| drop(Box::from_raw(compiled)))
        }));
    }
}

/// # Safety
/// `msg` must be the error string of a `CompileResult` and be freed only once.
#[no_mangle]
pub unsafe extern "C" fn free_compile_error(msg: *mut c_char) {
    if !msg.is_null() {
        drop(CString::from_raw(msg));
    }
}

/// # Safety
/// `result` must come from `render_compiled` and be freed only once.
#[no_mangle]
pub unsafe extern "C" fn free_result_cstring(result: ResultCString) {
    match result {
        ResultCString::Ok(ptr) | ResultCString::Err(ptr) => {
            if !ptr.is_null() {
                drop(CString::from_raw(ptr));
            }
        }
    }
}
