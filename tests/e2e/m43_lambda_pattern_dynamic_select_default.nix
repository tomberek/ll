let f = { localSystem, config, bootstrapFiles ? let table = { a = { x = 1; }; }; in table.${config} or 99, ... }: bootstrapFiles;
in f { localSystem = null; config = "b"; }
