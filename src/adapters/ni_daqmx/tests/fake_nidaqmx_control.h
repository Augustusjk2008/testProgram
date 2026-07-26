#pragma once

#include <cstdint>

namespace fake_nidaqmx {

void reset();
void setInputMask(std::uint32_t mask);
void failNextWrite(std::int32_t code, const char* message);

int createDoCalls();
int createDiCalls();
int startCalls();
int stopCalls();
int clearCalls();
int writeCalls();
int resetDeviceCalls();
std::uint32_t lastWrittenMask();
const char* lastDoPhysicalChannel();
const char* lastDiPhysicalChannel();
bool ioContractValid();

} // namespace fake_nidaqmx
