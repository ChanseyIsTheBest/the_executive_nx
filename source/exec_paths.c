/* exec_paths.c -- MIT licensed. See LICENSE. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "exec_paths.h"

static char d_dir[512], d_assets[600], d_files[600], d_lib[600], d_cxx[600];

int exec_paths_init(const char *argv0) {
  /* argv[0] is the romfs/sdmc path of the running .nro. Everything the port
   * needs sits beside it, so the folder name is the user's business. */
  const char *p = (argv0 && *argv0) ? argv0 : "sdmc:/switch/exec/executive_nx.nro";
  snprintf(d_dir, sizeof(d_dir), "%s", p);
  char *slash = strrchr(d_dir, '/');
  if (slash) *slash = 0; else snprintf(d_dir, sizeof(d_dir), "sdmc:/switch/exec");

  snprintf(d_assets, sizeof(d_assets), "%s/assets", d_dir);
  snprintf(d_files,  sizeof(d_files),  "%s/files",  d_dir);
  snprintf(d_lib,    sizeof(d_lib),    "%s/libexecutive_android.so", d_dir);
  snprintf(d_cxx,    sizeof(d_cxx),    "%s/libc++_shared.so",  d_dir);
  mkdir(d_files, 0777);

  struct stat st;
  if (stat(d_lib, &st) != 0) return -1;
  if (stat(d_cxx, &st) != 0) return -2;
  if (stat(d_assets, &st) != 0) return -3;
  return 0;
}

const char *exec_dir(void)        { return d_dir; }
const char *exec_assets_dir(void) { return d_assets; }
const char *exec_files_dir(void)  { return d_files; }
const char *exec_lib_path(void)   { return d_lib; }
const char *exec_cxx_path(void)   { return d_cxx; }
