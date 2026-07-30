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
    if (dynamic_cast<const pqxx::sql_error*>(&e)) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
    return {grpc::StatusCode::INTERNAL, e.what()};
}

}  // namespace evgrpc
