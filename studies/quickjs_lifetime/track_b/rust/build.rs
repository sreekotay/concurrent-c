use std::env;
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let study = manifest.parent().unwrap(); // track_b
    let libuv_build = env::var("LIBUV_DIR").unwrap_or_else(|_| {
        study
            .join("deps/libuv-build")
            .to_string_lossy()
            .into_owned()
    });
    let libuv_inc = env::var("LIBUV_INCLUDE").unwrap_or_else(|_| {
        study
            .join("deps/libuv/include")
            .to_string_lossy()
            .into_owned()
    });

    println!("cargo:rerun-if-env-changed=LIBUV_DIR");
    println!("cargo:rerun-if-env-changed=LIBUV_INCLUDE");
    println!("cargo:rerun-if-changed=uv_ffi.c");

    cc::Build::new()
        .file("uv_ffi.c")
        .include(&libuv_inc)
        .compile("tb_uv_ffi");

    println!("cargo:rustc-link-search=native={}", libuv_build);
    println!("cargo:rustc-link-lib=static=uv");
    println!("cargo:rustc-link-lib=static=tb_uv_ffi");
    if cfg!(target_os = "macos") {
        println!("cargo:rustc-link-lib=framework=CoreFoundation");
    } else if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=dl");
    }
}
