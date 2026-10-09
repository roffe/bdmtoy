#ifndef __HEADER_H
#define __HEADER_H

#pragma once
#include "../../../core/core.h"
#include "event.h"

using namespace System;

typedef unsigned int UINT;
typedef unsigned char BYTE;

namespace NameMeDLL
{
	// The bdmtoy host core for .NET. The core runs natively in here; the USB
	// link belongs to the C# side (UsbLink.cs, LibUsbDotNet 3): it sets
	// SendFrame, which gets every frame the core sends, and passes what the
	// adapter answers to Receive. Open the link before calling an operation.
	public ref class noNameGizmo : eventForwarder
	{
	private:
		static noNameGizmo ^thisptr;
		static void core_SharedSetup();

	public:
		delegate void FrameSender(array<Byte> ^frame);
		FrameSender ^SendFrame;

		static void managedProgress(int percentage);
		static void managedString(char *text);
		static void managedCallback();
		static void managedSend(array<Byte> ^frame);

		noNameGizmo();
		virtual ~noNameGizmo();

		// Data from the adapter (bulk IN, EP 0x81)
		void Receive(array<Byte> ^data, int len);

		// Adapter firmware version, major << 8 | minor; 0 for firmware before
		// 1.0, -1 if the adapter does not answer
		int FirmwareVersion();
		// Reset the adapter into its USB DFU bootloader (firmware 2.0+)
		bool EnterBootloader();

		String ^returnCoreVersion();
		const UINT returnNumberOfTargets();
		String ^returnTargetName(int index);
		String ^returnTargetInfo(int index);
		UINT returnTargetSizeFLASH(int index);
		UINT returnTargetSizeEEPROM(int index);
		UINT returnTargetSizeSRAM(int index);
		array<BYTE> ^returnBufferBytes(int index, bool eeprom);
		array<BYTE> ^returnBufferBytesSRAM(int index);

		void TAP_Dump(int index);
		void TAP_Flash(int index, array<BYTE> ^buffer);
		void TAP_ReadEeprom(int index);
		void TAP_WriteEeprom(int index, array<BYTE> ^buffer);
		void TAP_ReadSram(int index);
	};
}

#endif
