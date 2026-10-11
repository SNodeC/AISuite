import type {V2ImageUserInput, V2InputImageContentItem, RootMcpServerElicitationRequestParams} from "../src/protocol/generated.js";

const url: V2ImageUserInput = {type: "image", url: "data:image/png;base64,fixture"};
const file: V2ImageUserInput = {type: "image", fileId: "file-fixture"};
const content: V2InputImageContentItem = {type: "input_image", file_id: "file-fixture"};
const elicitation: RootMcpServerElicitationRequestParams = {
    serverName: "fixture", threadId: "thread", mode: "url", message: "Authenticate", elicitationId: "id", url: "https://example.invalid",
};
// @ts-expect-error Shared image discriminator is required alongside an alternative.
const missingType: V2ImageUserInput = {url: "data:image/png;base64,fixture"};
// @ts-expect-error One image source alternative is required.
const missingSource: V2ImageUserInput = {type: "image"};
// @ts-expect-error Discriminator constrains the complete alternative union.
const wrongType: V2ImageUserInput = {type: "text", fileId: "file-fixture"};
// @ts-expect-error Shared content discriminator is required.
const missingContentType: V2InputImageContentItem = {file_id: "file-fixture"};
// @ts-expect-error Shared MCP identity remains required alongside the mode alternatives.
const missingIdentity: RootMcpServerElicitationRequestParams = {mode: "url", message: "Authenticate", elicitationId: "id", url: "https://example.invalid"};
void [url, file, content, elicitation, missingType, missingSource, wrongType, missingContentType, missingIdentity];
