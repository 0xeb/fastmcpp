#pragma once
// Forward declarations for interactions test helpers

#include "interactions_fixture.hpp"

namespace fastmcpp
{

std::shared_ptr<server::Server> create_resource_interaction_server();

std::shared_ptr<server::Server> create_prompt_interaction_server();

std::shared_ptr<server::Server> create_meta_server();

std::shared_ptr<server::Server> create_output_schema_server();

std::shared_ptr<server::Server> create_content_type_server();

std::shared_ptr<server::Server> create_error_server();

std::shared_ptr<server::Server> create_unicode_server();

std::shared_ptr<server::Server> create_large_data_server();

std::shared_ptr<server::Server> create_special_cases_server();

std::shared_ptr<server::Server> create_pagination_server();

std::shared_ptr<server::Server> create_completion_server();

std::shared_ptr<server::Server> create_multi_content_server();

std::shared_ptr<server::Server> create_numeric_server();

std::shared_ptr<server::Server> create_bool_array_server();

std::shared_ptr<server::Server> create_concurrent_server();

std::shared_ptr<server::Server> create_mime_server();

std::shared_ptr<server::Server> create_empty_server();

std::shared_ptr<server::Server> create_schema_edge_server();

std::shared_ptr<server::Server> create_arg_variations_server();

std::shared_ptr<server::Server> create_annotations_server();

std::shared_ptr<server::Server> create_escape_server();

std::shared_ptr<server::Server> create_coercion_server();

std::shared_ptr<server::Server> create_prompt_args_server();

std::shared_ptr<server::Server> create_response_variations_server();

std::shared_ptr<server::Server> create_return_types_server();

std::shared_ptr<server::Server> create_resource_template_server();

std::shared_ptr<server::Server> create_coercion_params_server();

std::shared_ptr<server::Server> create_prompt_variations_server();

std::shared_ptr<server::Server> create_meta_variations_server();

std::shared_ptr<server::Server> create_error_edge_server();

std::shared_ptr<server::Server> create_resource_edge_server();

std::shared_ptr<server::Server> create_schema_description_server();

std::shared_ptr<server::Server> create_capabilities_server();

std::shared_ptr<server::Server> create_progress_server();

std::shared_ptr<server::Server> create_roots_server();

std::shared_ptr<server::Server> create_cancel_server();

std::shared_ptr<server::Server> create_logging_server();

std::shared_ptr<server::Server> create_image_server();

std::shared_ptr<server::Server> create_embedded_resource_server();

std::shared_ptr<server::Server> create_validation_server();

std::shared_ptr<server::Server> create_subscribe_server();

std::shared_ptr<server::Server> create_completion_edge_server();

} // namespace fastmcpp
