(derivation {
  name = "x";
  system = "x86_64-linux";
  builder = "/bin/sh";
  args = [ ./m75_path_copy_interp.txt ];
}).drvPath
