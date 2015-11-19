/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Symfiles are a generalization of Unix symlinks.

   A symfile when read behaves as though you took its contents and
   substituted them into the reiser4 naming system as the right hand side
   of an assignment, and then read that which you had assigned to it.

   A key issue for symfiles is how to implement writes through to
   subfiles.  In general, one must have some method of determining what
   of that which is written to the symfile is written to what subfile.
   This can be done by use of custom plugin methods written by users, or
   by using a few general methods we provide for those willing to endure
   the insertion of delimiters into what is read.

   Writing to symfiles without delimiters to denote what is written to
   what subfile is not supported by any plugins we provide in this
   release.  Our most sophisticated support for writes is that embodied
   by the invert plugin (see invert.c).

   A read only version of the /etc/passwd file might be
   constructed as a symfile whose contents are as follows:

   /etc/passwd/userlines/*

   or

   /etc/passwd/userlines/demidov+/etc/passwd/userlines/edward+/etc/passwd/userlines/reiser+/etc/passwd/userlines/root

   or

   /etc/passwd/userlines/(demidov+edward+reiser+root)

   A symfile with contents

   /filenameA+"(some text stored in the uninvertable symfile)+/filenameB

   will return when read

   The contents of filenameAsome text stored in the uninvertable symfileThe contents of filenameB

   and write of what has been read will not be possible to implement as
   an identity operation because there are no delimiters denoting the
   boundaries of what is to be written to what subfile.

   Note that one could make this a read/write symfile if one specified
   delimiters, and the write method understood those delimiters delimited
   what was written to subfiles.

   So, specifying the symfile in a manner that allows writes:

   /etc/passwd/userlines/demidov+"(
   )+/etc/passwd/userlines/edward+"(
   )+/etc/passwd/userlines/reiser+"(
   )+/etc/passwd/userlines/root+"(
   )

   or

   /etc/passwd/userlines/(demidov+"(
   )+edward+"(
   )+reiser+"(
   )+root+"(
   ))

   and the file demidov might be specified as:

   /etc/passwd/userlines/demidov/username+"(:)+/etc/passwd/userlines/demidov/password+"(:)+/etc/passwd/userlines/demidov/userid+"(:)+/etc/passwd/userlines/demidov/groupid+"(:)+/etc/passwd/userlines/demidov/gecos+"(:)+/etc/passwd/userlines/demidov/home+"(:)+/etc/passwd/userlines/demidov/shell

   or

   /etc/passwd/userlines/demidov/(username+"(:)+password+"(:)+userid+"(:)+groupid+"(:)+gecos+"(:)+home+"(:)+shell)

   Notice that if the file demidov has a carriage return in it, the
   parsing fails, but then if you put carriage returns in the wrong place
   in a normal /etc/passwd file it breaks things also.

   Note that it is forbidden to have no text between two interpolations
   if one wants to be able to define what parts of a write go to what
   subfiles referenced in an interpolation.

   If one wants to be able to add new lines by writing to the file, one
   must either write a custom plugin for /etc/passwd that knows how to
   name an added line, or one must use an invert, or one must use a more
   sophisticated symfile syntax that we are not planning to write for
   version 4.0.
*/
