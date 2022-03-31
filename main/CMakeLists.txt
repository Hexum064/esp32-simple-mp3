idf_component_register(SRCS "ssd1306.cpp" "ssd1306_spi.cpp" "ssd1306_i2c.cpp" "filenavi.cpp" "config.cpp" "I2SOutput.cpp" "Output.cpp" "main.cpp" 
                    INCLUDE_DIRS ".")
idf_build_set_property(CONFIG_FATFS_LONG_FILENAMES "y")         