#pragma once
// Note: the brief listed `<grpcpp/support/status_code_enum.h>` here, but that
// header only declares `grpc::StatusCode` (the enum). `grpc::Status` (the
// class used as the return type below) is declared in `<grpcpp/support/status.h>`,
// which transitively pulls in `status_code_enum.h`. Using the public header
// (which the gRPC source itself IWYU-marks as the include for both) keeps the
// include count to one and lets the `Status` type resolve correctly.
#include <grpcpp/support/status.h>
#include <stdexcept>

namespace evgrpc {

grpc::Status ToGrpcStatus(const std::exception& e);

}  // namespace evgrpc
