use std::slice;
use std::str;
use std::path::Path;
use std::ffi::{c_char, CString, CStr};

use minijinja::{Environment, Value, AutoEscape};
use std::error::Error;

#[repr(C)]
pub enum ResultCString {
    Ok(*mut c_char),
    Err(*mut c_char),
}

macro_rules! make_str {
    ($s:expr, $len:expr) => {
        unsafe { str::from_utf8_unchecked(slice::from_raw_parts($s as *const u8, $len)) }
    };
}

fn c_char_to_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(ptr).to_str().ok().map(|s| s.to_owned()) }
}


fn json_to_value(json_str: &str) -> Result<Value, serde_json::Error> {
    let json: Value = serde_json::from_str(json_str)?;
    Ok(Value::from(json))
}

#[no_mangle]
pub extern "C" fn render_template(
    template_source: *const c_char,
    template_source_len: usize,
    json_context: *const c_char,
    json_context_len: usize,
    template_path: *const c_char,
    autoescape: bool,
    undefined_behavior: *const c_char,
    autoescape_on: *const *const c_char,
    autoescape_on_count: usize,
) -> ResultCString {
    let template_str = make_str!(template_source, template_source_len);
    let json_str = make_str!(json_context, json_context_len);
    let mut template_path_str = c_char_to_string(template_path);
    if let Some(ref mut path) = template_path_str {
        if path.is_empty() {
            template_path_str = None;
        }
    }

    let mut undefined_behavior_str = c_char_to_string(undefined_behavior);
    if let Some(ref mut behavior) = undefined_behavior_str {
        if behavior.is_empty() {
            undefined_behavior_str = None;
        }
    }


    // Parse JSON context
    let ctx = match json_to_value(json_str) {
        Ok(c) => c,
        Err(e) => {
            let msg = CString::new(format!("Invalid JSON: {}", e)).unwrap();
            return ResultCString::Err(msg.into_raw());
        }
    };

    // Build environment
    let mut env = Environment::new();
    // Configure undefined behavior
    if let Some(behavior) = undefined_behavior_str {
        match behavior.as_str() {
            "strict" => {
                env.set_undefined_behavior(minijinja::UndefinedBehavior::Strict);
            }
            "semistrict" => {
                env.set_undefined_behavior(minijinja::UndefinedBehavior::SemiStrict);
            }
            "chainable" => {
                env.set_undefined_behavior(minijinja::UndefinedBehavior::Chainable);
            }
            "lenient" => {
                env.set_undefined_behavior(minijinja::UndefinedBehavior::Lenient);
            }
            _ => {
                env.set_undefined_behavior(minijinja::UndefinedBehavior::Lenient);
            }
        }
    }

    // Load templates if template_path is provided
    if let Some(ref path) = template_path_str {
        let p = Path::new(path);
        if p.is_dir() {
            env.set_loader(minijinja::path_loader(p));
        } else if p.is_file() {
            if let Some(parent) = p.parent() {
                env.set_loader(minijinja::path_loader(parent));
            }
        }
    }

    env.set_auto_escape_callback(|_| AutoEscape::Html);

    // Configure autoescape
    if !autoescape && autoescape_on_count == 0 {
        env.set_auto_escape_callback(|_| AutoEscape::None);
    } else {
        if autoescape_on_count > 0 {
            unsafe {
                let slice: &[*const c_char] = std::slice::from_raw_parts(autoescape_on, autoescape_on_count);
                let exts: Vec<String> = slice
                    .iter()
                    .filter_map(|&ptr| (!ptr.is_null()).then(|| CStr::from_ptr(ptr).to_string_lossy().into_owned()))
                    .collect();

                env.set_auto_escape_callback(move |name| {
                    if exts.iter().any(|ext| name.ends_with(ext)) {
                        return AutoEscape::Html;
                    }

                    AutoEscape::None
                });
            }
        }
    }

    // Render
    let result = if template_path_str.is_some() {
        env.get_template(template_str).and_then(|tmpl| tmpl.render(&ctx))
    } else {
        // Inline template only
        env.render_str(template_str, &ctx)
    };

    // Return
    match result {
        Ok(output) => {
            let s = CString::new(output).unwrap();
            ResultCString::Ok(s.into_raw())
        }
         Err(err) => {
        let mut msg = format!("MiniJinja render error: {:?}\n", err);

        // Add source chain
        let mut source = err.source();
        while let Some(s) = source {
            msg.push_str(&format!("Caused by: {}\n", s));
            source = s.source();
        }

        let c_msg = CString::new(msg).unwrap_or_else(|_| {
            CString::new("MiniJinja render error (message contained null byte)").unwrap()
        });

        ResultCString::Err(c_msg.into_raw())
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn free_result_cstring(result: ResultCString) {
    match result {
        ResultCString::Ok(ptr) | ResultCString::Err(ptr) => {
            if !ptr.is_null() {
                let _ = CString::from_raw(ptr);
            }
        }
    }
}