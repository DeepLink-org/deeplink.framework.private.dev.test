from common.utils import *

class OpModule(torch.nn.Module):
    def forward(self, a, b, c):
        res_dim = torch.ops.aten.mean.dim(a, b, c)
        return res_dim

model = OpModule()
args = parse_args()
compiled_model = compile_model(model, args.backend, args.dynamic)


class TestMean():
    @pytest.mark.parametrize("dtype", [torch.float32])
    @pytest.mark.parametrize("sizes", [Size((5,), (5, 3)), Size((3, 5), (5, 3)), Size((2, 3, 4), (2, 4))])
    @pytest.mark.parametrize("keepdim", [True, False])
    @pytest.mark.parametrize("compiled_model", compiled_model)
    def test_torch_mean(self, sizes, keepdim, dtype, compiled_model):
        device = get_device()
        size = sizes.dynamic if compiled_model.dynamic else sizes.static
        input1 = torch.randn(size, dtype=dtype)
        dim = [0] if len(size) < 2 else [0, 1]
        keepdim = True if len(size) <= 2 else keepdim

        dicp_input1 = input1.to(device)

        output = model(input1, dim, keepdim)
        dynamo.reset()
        update_dynamo_config(compiled_model.dynamic)
        dicp_output = compiled_model.model(dicp_input1, dim, keepdim)

        assert torch.allclose(output, dicp_output.cpu(), equal_nan=True)
