#!/bin/bash

. lib.sh

#
# *** Description ***
#
# generate primary header with too large keyslots_size set in config section
# (iow config.keyslots_size = data_offset - keyslots_offset + 512)
#
# secondary header is corrupted on purpose as well
#

# $1 full target dir
# $2 full source luks2 image

function prepare()
{
	cp $SRC_IMG $TGT_IMG
	test -d $TMPDIR || mkdir $TMPDIR
	read_luks2_json0 $TGT_IMG $TMPDIR/json0
	read_luks2_bin_hdr0 $TGT_IMG $TMPDIR/hdr0
	read_luks2_bin_hdr1 $TGT_IMG $TMPDIR/hdr1
}

function generate()
{
	# make area 7 being included in area 6
	OFFS=$((2*LUKS2_HDR_SIZE*512))
	json_str=$(jq -c --arg off $OFFS '.config.keyslots_size = (.segments."0".offset | tonumber - ($off | tonumber) + 4096 | tostring)' $TMPDIR/json0)
	test -n "$json_str" || exit 2
	# [.keyslots[].area.offset | tonumber] | max | tostring ---> max offset in keyslot areas
	test ${#json_str} -lt $((LUKS2_JSON_SIZE*512)) || exit 2

	write_luks2_json "$json_str" $TMPDIR/json0

	merge_bin_hdr_with_json $TMPDIR/hdr0 $TMPDIR/json0 $TMPDIR/area0
	erase_checksum $TMPDIR/area0
	chks0=$(calc_sha256_checksum_file $TMPDIR/area0)
	write_checksum $chks0 $TMPDIR/area0
	write_luks2_hdr0 $TMPDIR/area0 $TGT_IMG
	kill_bin_hdr $TMPDIR/hdr1
	write_luks2_hdr1 $TMPDIR/hdr1 $TGT_IMG
}

function check()
{
	read_luks2_bin_hdr1 $TGT_IMG $TMPDIR/hdr_res1
	local str_res1=$(head -c 6 $TMPDIR/hdr_res1)
	test "$str_res1" = "VACUUM" || exit 2

	read_luks2_json0 $TGT_IMG $TMPDIR/json_res0
	jq -c --arg off $OFFS 'if .config.keyslots_size != ( .segments."0".offset | tonumber - ($off | tonumber) + 4096 | tostring )
	       then error("Unexpected value in result json") else empty end' $TMPDIR/json_res0 || exit 5
}

function cleanup()
{
	rm -f $TMPDIR/*
	rm -fd $TMPDIR
}

test $# -eq 2 || exit 1

TGT_IMG=$1/$(test_img_name $0)
SRC_IMG=$2

prepare
generate
check
cleanup
