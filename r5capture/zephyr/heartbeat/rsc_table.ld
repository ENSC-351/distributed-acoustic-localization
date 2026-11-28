/* Linux remoteproc finds the resource table by this section name */
SECTION_PROLOGUE(.resource_table,, SUBALIGN(4))
{
	KEEP(*(.resource_table*))
} GROUP_LINK_IN(RSC_TABLE)
