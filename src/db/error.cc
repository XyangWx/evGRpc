#include "db/error.h"
#include <pqxx/pqxx>

namespace evgrpc {

grpc::Status ToGrpcStatus(const std::exception& e) {
    if (dynamic_cast<const pqxx::unique_violation*>(&e)) {
        return {grpc::StatusCode::ALREADY_EXISTS, e.what()};
    }
    if (dynamic_cast<const pqxx::foreign_key_violation*>(&e)) {
        return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    if (dynamic_cast<const pqxx::data_exception*>(&e)) {
        return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    // not_null_violation is a subclass of sql_error (NOT data_exception).
    // Map to INVALID_ARGUMENT: the user input violated a required column,
    // so it's a bad-request, not a server crash.
    if (dynamic_cast<const pqxx::not_null_violation*>(&e)) {
        return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    if (dynamic_cast<const pqxx::sql_error*>(&e)) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
    return {grpc::StatusCode::INTERNAL, e.what()};
}

}  // namespace evgrpc
