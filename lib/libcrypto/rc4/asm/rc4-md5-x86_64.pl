#!/usr/bin/env perl
#
# ====================================================================
# Written by Andy Polyakov <appro@openssl.org> for the OpenSSL
# project. The module is, however, dual licensed under OpenSSL and
# CRYPTOGAMS licenses depending on where you obtain it. For further
# details see http://www.openssl.org/~appro/cryptogams/.
# ====================================================================

# June 2011
#
# This is RC4+MD5 "stitch" implementation. The idea, as spelled in
# http://download.intel.com/design/intarch/papers/323686.pdf, is that
# since both algorithms exhibit instruction-level parallelism, ILP,
# below theoretical maximum, interleaving them would allow to utilize
# processor resources better and achieve better performance. RC4
# instruction sequence is virtually identical to rc4-x86_64.pl, which
# is heavily based on submission by Maxim Perminov, Maxim Locktyukhin
# and Jim Guilford of Intel. MD5 is fresh implementation aiming to
# minimize register usage, which was used as "main thread" with RC4
# weaved into it, one RC4 round per one MD5 round. In addition to the
# stiched subroutine the script can generate standalone replacement
# md5_block_asm_data_order and RC4. Below are performance numbers in
# cycles per processed byte, less is better, for these the standalone
# subroutines, sum of them, and stitched one:
#
#		RC4	MD5	RC4+MD5	stitch	gain
# Opteron	6.5(*)	5.4	11.9	7.0	+70%(*)
# Core2		6.5	5.8	12.3	7.7	+60%
# Westmere	4.3	5.2	9.5	7.0	+36%
# Sandy Bridge	4.2	5.5	9.7	6.8	+43%
# Atom		9.3	6.5	15.8	11.1	+42%
#
# (*)	rc4-x86_64.pl delivers 5.3 on Opteron, so real improvement
#	is +53%...

my ($rc4,$md5)=(1,1);	# what to generate?
my $D="#" if (!$md5);	# if set to "#", MD5 is stitched into RC4(),
			# but its result is discarded. Idea here is
			# to be able to use 'openssl speed rc4' for
			# benchmarking the stitched subroutine... 

my $flavour = shift;
my $output  = shift;
if ($flavour =~ /\./) { $output = $flavour; undef $flavour; }

$0 =~ m/(.*[\/\\])[^\/\\]+$/; my $dir=$1; my $xlate;
( $xlate="${dir}x86_64-xlate.pl" and -f $xlate ) or
( $xlate="${dir}../../perlasm/x86_64-xlate.pl" and -f $xlate) or
die "can't locate x86_64-xlate.pl";

open OUT,"| \"$^X\" $xlate $flavour $output";
*STDOUT=*OUT;

my ($dat,$in0,$out,$ctx,$inp,$len, $func,$nargs);

if ($rc4 && !$md5) {
  ($dat,$len,$in0,$out) = ("%rdi","%rsi","%rdx","%rcx");
  $func="RC4";				$nargs=4;
} elsif ($md5 && !$rc4) {
  ($ctx,$inp,$len) = ("%rdi","%rsi","%rdx");
  $func="md5_block_asm_data_order";	$nargs=3;
} else {
  ($dat,$in0,$out,$ctx,$inp,$len) = ("%rdi","%rsi","%rdx","%rcx","%r8","%r9");
  $func="rc4_md5_enc";			$nargs=6;
  # void rc4_md5_enc(
  #		RC4_KEY *key,		#
  #		const void *in0,	# RC4 input
  #		void *out,		# RC4 output
  #		MD5_CTX *ctx,		#
  #		const void *inp,	# MD5 input
  #		size_t len);		# number of 64-byte blocks
}

my @K=(	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
	0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
	0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
	0x6b901122,0xfd987193,0xa679438e,0x49b40821,

	0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
	0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
	0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,

	0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
	0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
	0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
	0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,

	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
	0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
	0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
	0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391	);

my @V=("%r8d","%r9d","%r10d","%r11d");	# MD5 registers
my $tmp="%r12d";

my @XX=("%rbp","%rsi");			# RC4 registers
my @TX=("%rax","%rbx");
my $YY="%rcx";
my $TY="%rdx";

my $MOD=32;				# 16, 32 or 64

$code.=<<___;
.text
.align 16

.globl	$func
.type	$func,\@function,$nargs
$func:
	_CET_ENDBR
	cmp	\$0,$len
	je	.Labort
	push	%rbx
	push	%rbp
	push	%r12
	push	%r13
	push	%r14
	push	%r15
	sub	\$40,%rsp
.Lbody:
___
if ($rc4) {
$code.=<<___;
$D#md5#	mov	$ctx,%r11		# reassign arguments
	mov	$len,%r12
	mov	$in0,%r13
	mov	$out,%r14
$D#md5#	mov	$inp,%r15
___
    $ctx="%r11"	if ($md5);		# reassign arguments
    $len="%r12";
    $in0="%r13";
    $out="%r14";
    $inp="%r15"	if ($md5);
    $inp=$in0	if (!$md5);
$code.=<<___;
	xor	$XX[0],$XX[0]
	xor	$YY,$YY

	lea	8($dat),$dat
	mov	-8($dat),$XX[0]#b
	mov	-4($dat),$YY#b

	inc	$XX[0]#b
	sub	$in0,$out
	movl	($dat,$XX[0],4),$TX[0]#d
___
$code.=<<___ if (!$md5);
	xor	$TX[1],$TX[1]
	test	\$-128,$len
	jz	.Loop1
	sub	$XX[0],$TX[1]
	and	\$`$MOD-1`,$TX[1]
	jz	.Loop${MOD}_is_hot
	sub	$TX[1],$len
.Loop${MOD}_warmup:
	add	$TX[0]#b,$YY#b
	movl	($dat,$YY,4),$TY#d
	movl	$TX[0]#d,($dat,$YY,4)
	movl	$TY#d,($dat,$XX[0],4)
	add	$TY#b,$TX[0]#b
	inc	$XX[0]#b
	movl	($dat,$TX[0],4),$TY#d
	movl	($dat,$XX[0],4),$TX[0]#d
	xorb	($in0),$TY#b
	movb	$TY#b,($out,$in0)
	lea	1($in0),$in0
	dec	$TX[1]
	jnz	.Loop${MOD}_warmup

	mov	$YY,$TX[1]
	xor	$YY,$YY
	mov	$TX[1]#b,$YY#b

.Loop${MOD}_is_hot:
	mov	$len,32(%rsp)		# save original $len
	shr	\$6,$len		# number of 64-byte blocks
___
  if ($D && !$md5) {			# stitch in dummy MD5
    $md5=1;
    $ctx="%r11";
    $inp="%r15";
    $code.=<<___;
	mov	%rsp,$ctx
	mov	$in0,$inp
___
  }
}
$code.=<<___;
#rc4#	add	$TX[0]#b,$YY#b
#rc4#	lea	($dat,$XX[0],4),$XX[1]
	shl	\$6,$len
	add	$inp,$len		# pointer to the end of input
	mov	$len,16(%rsp)

#md5#	mov	$ctx,24(%rsp)		# save pointer to MD5_CTX
#md5#	mov	0*4($ctx),$V[0]		# load current hash value from MD5_CTX
#md5#	mov	1*4($ctx),$V[1]
#md5#	mov	2*4($ctx),$V[2]
#md5#	mov	3*4($ctx),$V[3]
	jmp	.Loop

.align	16
.Loop:
#md5#	mov	$V[0],0*4(%rsp)		# put aside current hash value
#md5#	mov	$V[1],1*4(%rsp)
#md5#	mov	$V[2],2*4(%rsp)
#md5#	mov	$V[3],$tmp		# forward reference
#md5#	mov	$V[3],3*4(%rsp)
___

sub R0 {
  my ($i,$a,$b,$c,$d)=@_;
  my @rot0=(7,12,17,22);
  my $j=$i%16;
  my $k=$i%$MOD;
  my $xmm="%xmm".($j&1);
    $code.="	movdqu	($in0),%xmm2\n"		if ($rc4 && $j==15);
    $code.="	add	\$$MOD,$XX[0]#b\n"	if ($rc4 && $j==15 && $k==$MOD-1);
    $code.="	pxor	$xmm,$xmm\n"		if ($rc4 && $j<=1);
    $code.=<<___;
#rc4#	movl	($dat,$YY,4),$TY#d
#md5#	xor	$c,$tmp
#rc4#	movl	$TX[0]#d,($dat,$YY,4)
#md5#	and	$b,$tmp
#md5#	add	4*`$j`($inp),$a
#rc4#	add	$TY#b,$TX[0]#b
#rc4#	movl	`4*(($k+1)%$MOD)`(`$k==$MOD-1?"$dat,$XX[0],4":"$XX[1]"`),$TX[1]#d
#md5#	add	\$$K[$i],$a
#md5#	xor	$d,$tmp
#rc4#	movz	$TX[0]#b,$TX[0]#d
#rc4#	movl	$TY#d,4*$k($XX[1])
#md5#	add	$tmp,$a
#rc4#	add	$TX[1]#b,$YY#b
#md5#	rol	\$$rot0[$j%4],$a
#md5#	mov	`$j==15?"$b":"$c"`,$tmp		# forward reference
#rc4#	pinsrw	\$`($j>>1)&7`,($dat,$TX[0],4),$xmm\n
#md5#	add	$b,$a
___
    $code.=<<___ if ($rc4 && $j==15 && $k==$MOD-1);
	mov	$YY,$XX[1]
	xor	$YY,$YY				# keyword to partial register
	mov	$XX[1]#b,$YY#b
	lea	($dat,$XX[0],4),$XX[1]
___
    $code.=<<___ if ($rc4 && $j==15);
	psllq	\$8,%xmm1
	pxor	%xmm0,%xmm2
	pxor	%xmm1,%xmm2
___
}
sub R1 {
  my ($i,$a,$b,$c,$d)=@_;
  my @rot1=(5,9,14,20);
  my $j=$i%16;
  my $k=$i%$MOD;
  my $xmm="%xmm".($j&1);
    $code.="	movdqu	16($in0),%xmm3\n"	if ($rc4 && $j==15);
    $code.="	add	\$$MOD,$XX[0]#b\n"	if ($rc4 && $j==15 && $k==$MOD-1);
    $code.="	pxor	$xmm,$xmm\n"		if ($rc4 && $j<=1);
    $code.=<<___;
#rc4#	movl	($dat,$YY,4),$TY#d
#md5#	xor	$b,$tmp
#rc4#	movl	$TX[0]#d,($dat,$YY,4)
#md5#	and	$d,$tmp
#md5#	add	4*`((1+5*$j)%16)`($inp),$a
#rc4#	add	$TY#b,$TX[0]#b
#rc4#	movl	`4*(($k+1)%$MOD)`(`$k==$MOD-1?"$dat,$XX[0],4":"$XX[1]"`),$TX[1]#d
#md5#	add	\$$K[$i],$a
#md5#	xor	$c,$tmp
#rc4#	movz	$TX[0]#b,$TX[0]#d
#rc4#	movl	$TY#d,4*$k($XX[1])
#md5#	add	$tmp,$a
#rc4#	add	$TX[1]#b,$YY#b
#md5#	rol	\$$rot1[$j%4],$a
#md5#	mov	`$j==15?"$c":"$b"`,$tmp		# forward reference
#rc4#	pinsrw	\$`($j>>1)&7`,($dat,$TX[0],4),$xmm\n
#md5#	add	$b,$a
___
    $code.=<<___ if ($rc4 && $j==15 && $k==$MOD-1);
	mov	$YY,$XX[1]
	xor	$YY,$YY				# keyword to partial register
	mov	$XX[1]#b,$YY#b
	lea	($dat,$XX[0],4),$XX[1]
___
    $code.=<<___ if ($rc4 && $j==15);
	psllq	\$8,%xmm1
	pxor	%xmm0,%xmm3
	pxor	%xmm1,%xmm3
___
}
sub R2 {
  my ($i,$a,$b,$c,$d)=@_;
  my @rot2=(4,11,16,23);
  my $j=$i%16;
  my $k=$i%$MOD;
  my $xmm="%xmm".($j&1);
    $code.="	movdqu	32($in0),%xmm4\n"	if ($rc4 && $j==15);
    $code.="	add	\$$MOD,$XX[0]#b\n"	if ($rc4 && $j==15 && $k==$MOD-1);
    $code.="	pxor	$xmm,$xmm\n"		if ($rc4 && $j<=1);
    $code.=<<___;
#rc4#	movl	($dat,$YY,4),$TY#d
#md5#	xor	$c,$tmp
#rc4#	movl	$TX[0]#d,($dat,$YY,4)
#md5#	xor	$b,$tmp
#md5#	add	4*`((5+3*$j)%16)`($inp),$a
#rc4#	add	$TY#b,$TX[0]#b
#rc4#	movl	`4*(($k+1)%$MOD)`(`$k==$MOD-1?"$dat,$XX[0],4":"$XX[1]"`),$TX[1]#d
#md5#	add	\$$K[$i],$a
#rc4#	movz	$TX[0]#b,$TX[0]#d
#md5#	add	$tmp,$a
#rc4#	movl	$TY#d,4*$k($XX[1])
#rc4#	add	$TX[1]#b,$YY#b
#md5#	rol	\$$rot2[$j%4],$a
#md5#	mov	`$j==15?"\\\$-1":"$c"`,$tmp	# forward reference
#rc4#	pinsrw	\$`($j>>1)&7`,($dat,$TX[0],4),$xmm\n
#md5#	add	$b,$a
___
    $code.=<<___ if ($rc4 && $j==15 && $k==$MOD-1);
	mov	$YY,$XX[1]
	xor	$YY,$YY				# keyword to partial register
	mov	$XX[1]#b,$YY#b
	lea	($dat,$XX[0],4),$XX[1]
___
    $code.=<<___ if ($rc4 && $j==15);
	psllq	\$8,%xmm1
	pxor	%xmm0,%xmm4
	pxor	%xmm1,%xmm4
___
}
sub R3 {
  my ($i,$a,$b,$c,$d)=@_;
  my @rot3=(6,10,15,21);
  my $j=$i%16;
  my $k=$i%$MOD;
  my $xmm="%xmm".($j&1);
    $code.="	movdqu	48($in0),%xmm5\n"	if ($rc4 && $j==15);
    $code.="	add	\$$MOD,$XX[0]#b\n"	if ($rc4 && $j==15 && $k==$MOD-1);
    $code.="	pxor	$xmm,$xmm\n"		if ($rc4 && $j<=1);
    $code.=<<___;
#rc4#	movl	($dat,$YY,4),$TY#d
#md5#	xor	$d,$tmp
#rc4#	movl	$TX[0]#d,($dat,$YY,4)
#md5#	or	$b,$tmp
#md5#	add	4*`((7*$j)%16)`($inp),$a
#rc4#	add	$TY#b,$TX[0]#b
#rc4#	movl	`4*(($k+1)%$MOD)`(`$k==$MOD-1?"$dat,$XX[0],4":"$XX[1]"`),$TX[1]#d
#md5#	add	\$$K[$i],$a
#rc4#	movz	$TX[0]#b,$TX[0]#d
#md5#	xor	$c,$tmp
#rc4#	movl	$TY#d,4*$k($XX[1])
#md5#	add	$tmp,$a
#rc4#	add	$TX[1]#b,$YY#b
#md5#	rol	\$$rot3[$j%4],$a
#md5#	mov	\$-1,$tmp			# forward reference
#rc4#	pinsrw	\$`($j>>1)&7`,($dat,$TX[0],4),$xmm\n
#md5#	add	$b,$a
___
    $code.=<<___ if ($rc4 && $j==15);
	mov	$XX[0],$XX[1]
	xor	$XX[0],$XX[0]			# keyword to partial register
	mov	$XX[1]#b,$XX[0]#b
	mov	$YY,$XX[1]
	xor	$YY,$YY				# keyword to partial register
	mov	$XX[1]#b,$YY#b
	lea	($dat,$XX[0],4),$XX[1]
	psllq	\$8,%xmm1
	pxor	%xmm0,%xmm5
	pxor	%xmm1,%xmm5
___
}

my $i=0;
for(;$i<16;$i++) { R0($i,@V); unshift(@V,pop(@V)); push(@TX,shift(@TX)); }
for(;$i<32;$i++) { R1($i,@V); unshift(@V,pop(@V)); push(@TX,shift(@TX)); }
for(;$i<48;$i++) { R2($i,@V); unshift(@V,pop(@V)); push(@TX,shift(@TX)); }
for(;$i<64;$i++) { R3($i,@V); unshift(@V,pop(@V)); push(@TX,shift(@TX)); }

$code.=<<___;
#md5#	add	0*4(%rsp),$V[0]		# accumulate hash value
#md5#	add	1*4(%rsp),$V[1]
#md5#	add	2*4(%rsp),$V[2]
#md5#	add	3*4(%rsp),$V[3]

#rc4#	movdqu	%xmm2,($out,$in0)	# write RC4 output
#rc4#	movdqu	%xmm3,16($out,$in0)
#rc4#	movdqu	%xmm4,32($out,$in0)
#rc4#	movdqu	%xmm5,48($out,$in0)
#md5#	lea	64($inp),$inp
#rc4#	lea	64($in0),$in0
	cmp	16(%rsp),$inp		# are we done?
	jb	.Loop

#md5#	mov	24(%rsp),$len		# restore pointer to MD5_CTX
#rc4#	sub	$TX[0]#b,$YY#b		# correct $YY
#md5#	mov	$V[0],0*4($len)		# write MD5_CTX
#md5#	mov	$V[1],1*4($len)
#md5#	mov	$V[2],2*4($len)
#md5#	mov	$V[3],3*4($len)
___
$code.=<<___ if ($rc4 && (!$md5 || $D));
	mov	32(%rsp),$len		# restore original $len
	and	\$63,$len		# remaining bytes
	jnz	.Loop1
	jmp	.Ldone
	
.align	16
.Loop1:
	add	$TX[0]#b,$YY#b
	movl	($dat,$YY,4),$TY#d
	movl	$TX[0]#d,($dat,$YY,4)
	movl	$TY#d,($dat,$XX[0],4)
	add	$TY#b,$TX[0]#b
	inc	$XX[0]#b
	movl	($dat,$TX[0],4),$TY#d
	movl	($dat,$XX[0],4),$TX[0]#d
	xorb	($in0),$TY#b
	movb	$TY#b,($out,$in0)
	lea	1($in0),$in0
	dec	$len
	jnz	.Loop1

.Ldone:
___
$code.=<<___;
#rc4#	sub	\$1,$XX[0]#b
#rc4#	movl	$XX[0]#d,-8($dat)
#rc4#	movl	$YY#d,-4($dat)

	mov	40(%rsp),%r15
	mov	48(%rsp),%r14
	mov	56(%rsp),%r13
	mov	64(%rsp),%r12
	mov	72(%rsp),%rbp
	mov	80(%rsp),%rbx
	lea	88(%rsp),%rsp
.Lepilogue:
.Labort:
	ret
.size $func,.-$func
___

if ($rc4 && $D) {	# sole purpose of this section is to provide
			# option to use the generated module as drop-in
			# replacement for rc4-x86_64.pl for debugging
			# and testing purposes...
my ($idx,$ido)=("%r8","%r9");
my ($dat,$len,$inp)=("%rdi","%rsi","%rdx");

$code.=<<___;
.globl	RC4_set_key
.type	RC4_set_key,\@function,3
.align	16
RC4_set_key:
	_CET_ENDBR
	lea	8($dat),$dat
	lea	($inp,$len),$inp
	neg	$len
	mov	$len,%rcx
	xor	%eax,%eax
	xor	$ido,$ido
	xor	%r10,%r10
	xor	%r11,%r11
	jmp	.Lw1stloop

.align	16
.Lw1stloop:
	mov	%eax,($dat,%rax,4)
	add	\$1,%al
	jnc	.Lw1stloop

	xor	$ido,$ido
	xor	$idx,$idx
.align	16
.Lw2ndloop:
	mov	($dat,$ido,4),%r10d
	add	($inp,$len,1),$idx#b
	add	%r10b,$idx#b
	add	\$1,$len
	mov	($dat,$idx,4),%r11d
	cmovz	%rcx,$len
	mov	%r10d,($dat,$idx,4)
	mov	%r11d,($dat,$ido,4)
	add	\$1,$ido#b
	jnc	.Lw2ndloop

	xor	%eax,%eax
	mov	%eax,-8($dat)
	mov	%eax,-4($dat)
	ret
.size	RC4_set_key,.-RC4_set_key
___
}

sub reg_part {
my ($reg,$conv)=@_;
    if ($reg =~ /%r[0-9]+/)     { $reg .= $conv; }
    elsif ($conv eq "b")        { $reg =~ s/%[er]([^x]+)x?/%$1l/;       }
    elsif ($conv eq "w")        { $reg =~ s/%[er](.+)/%$1/;             }
    elsif ($conv eq "d")        { $reg =~ s/%[er](.+)/%e$1/;            }
    return $reg;
}

$code =~ s/(%[a-z0-9]+)#([bwd])/reg_part($1,$2)/gem;
$code =~ s/\`([^\`]*)\`/eval $1/gem;
$code =~ s/pinsrw\s+\$0,/movd	/gm;

$code =~ s/#md5#//gm	if ($md5);
$code =~ s/#rc4#//gm	if ($rc4);

print $code;

close STDOUT;
