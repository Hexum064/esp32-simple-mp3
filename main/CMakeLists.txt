idf_component_register(SRCS "filenavi.cpp" "config.cpp" "I2SOutput.cpp" "Output.cpp" "main.cpp"
                    INCLUDE_DIRS ".")
idf_build_set_property(CONFIG_FATFS_LONG_FILENAMES "y")         