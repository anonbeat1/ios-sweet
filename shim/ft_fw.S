/* FT3680 app firmware for the Spacewar touch probe: build-spacewar.sh turns
 * the Focaltech .i (FT_FW_I) into out/sw/ft_fw.bin. Not in git. */
	.section .rodata
	.balign 16
	.global ft_fw, ft_fw_end
ft_fw:
	.incbin "out/sw/ft_fw.bin"
ft_fw_end:
