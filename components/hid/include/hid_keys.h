#pragma once

/* HID usage IDs of the keyboard page (0x07) and the modifier bits of the
 * boot keyboard report, from the USB HID Usage Tables. The USB link and
 * the Bluetooth link use the same codes. */

#define HID_MOD_LEFT_CTRL   0x01
#define HID_MOD_LEFT_SHIFT  0x02
#define HID_MOD_LEFT_ALT    0x04
#define HID_MOD_LEFT_GUI    0x08

#define HID_KEY_ENTER       0x28
#define HID_KEY_CAPS_LOCK   0x39

/* Initializer of a table [128][2]: for each ASCII code, 1 if the key
 * needs Shift, and the usage ID (0: no key). US layout. */
#define HID_ASCII_TO_KEY \
    { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 },     /* 0x00 to 0x03 */ \
    { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 },     /* 0x04 to 0x07 */ \
    { 0, 0x2A }, { 0, 0x2B }, { 0, 0x28 }, { 0, 0x00 },     /* BS, TAB, LF */ \
    { 0, 0x00 }, { 0, 0x28 }, { 0, 0x00 }, { 0, 0x00 },     /* CR */ \
    { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 },     /* 0x10 to 0x13 */ \
    { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 },     /* 0x14 to 0x17 */ \
    { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 }, { 0, 0x29 },     /* ESC */ \
    { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 }, { 0, 0x00 },     /* 0x1C to 0x1F */ \
    { 0, 0x2C }, { 1, 0x1E }, { 1, 0x34 }, { 1, 0x20 },     /* space ! " # */ \
    { 1, 0x21 }, { 1, 0x22 }, { 1, 0x24 }, { 0, 0x34 },     /* $ % & ' */ \
    { 1, 0x26 }, { 1, 0x27 }, { 1, 0x25 }, { 1, 0x2E },     /* ( ) * + */ \
    { 0, 0x36 }, { 0, 0x2D }, { 0, 0x37 }, { 0, 0x38 },     /* , - . / */ \
    { 0, 0x27 }, { 0, 0x1E }, { 0, 0x1F }, { 0, 0x20 },     /* 0 1 2 3 */ \
    { 0, 0x21 }, { 0, 0x22 }, { 0, 0x23 }, { 0, 0x24 },     /* 4 5 6 7 */ \
    { 0, 0x25 }, { 0, 0x26 }, { 1, 0x33 }, { 0, 0x33 },     /* 8 9 : ; */ \
    { 1, 0x36 }, { 0, 0x2E }, { 1, 0x37 }, { 1, 0x38 },     /* < = > ? */ \
    { 1, 0x1F }, { 1, 0x04 }, { 1, 0x05 }, { 1, 0x06 },     /* @ A B C */ \
    { 1, 0x07 }, { 1, 0x08 }, { 1, 0x09 }, { 1, 0x0A },     /* D E F G */ \
    { 1, 0x0B }, { 1, 0x0C }, { 1, 0x0D }, { 1, 0x0E },     /* H I J K */ \
    { 1, 0x0F }, { 1, 0x10 }, { 1, 0x11 }, { 1, 0x12 },     /* L M N O */ \
    { 1, 0x13 }, { 1, 0x14 }, { 1, 0x15 }, { 1, 0x16 },     /* P Q R S */ \
    { 1, 0x17 }, { 1, 0x18 }, { 1, 0x19 }, { 1, 0x1A },     /* T U V W */ \
    { 1, 0x1B }, { 1, 0x1C }, { 1, 0x1D }, { 0, 0x2F },     /* X Y Z [ */ \
    { 0, 0x31 }, { 0, 0x30 }, { 1, 0x23 }, { 1, 0x2D },     /* \ ] ^ _ */ \
    { 0, 0x35 }, { 0, 0x04 }, { 0, 0x05 }, { 0, 0x06 },     /* ` a b c */ \
    { 0, 0x07 }, { 0, 0x08 }, { 0, 0x09 }, { 0, 0x0A },     /* d e f g */ \
    { 0, 0x0B }, { 0, 0x0C }, { 0, 0x0D }, { 0, 0x0E },     /* h i j k */ \
    { 0, 0x0F }, { 0, 0x10 }, { 0, 0x11 }, { 0, 0x12 },     /* l m n o */ \
    { 0, 0x13 }, { 0, 0x14 }, { 0, 0x15 }, { 0, 0x16 },     /* p q r s */ \
    { 0, 0x17 }, { 0, 0x18 }, { 0, 0x19 }, { 0, 0x1A },     /* t u v w */ \
    { 0, 0x1B }, { 0, 0x1C }, { 0, 0x1D }, { 1, 0x2F },     /* x y z { */ \
    { 1, 0x31 }, { 1, 0x30 }, { 1, 0x35 }, { 0, 0x4C }      /* | } ~ DEL */
