EX_SRCS   := ftl/ftl_core.c ftl/nand_sim.c
# 100 us tick so vTaskDelay can model NAND timing; geometry 128 blocks x 64 pages
EX_CFLAGS := -Iftl -DconfigTICK_RATE_HZ=10000 -DFTL_BLOCKS=128 -DFTL_PPB=64
